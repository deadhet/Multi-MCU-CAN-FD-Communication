"""Captures MCU1's UART link from Renode (TCP) or the board (serial), decodes every frame, follows
the link state, and optionally checks everything against MCU1's debug log.

    uv run python -m noccarc_dashboard.capture --port 3456 --lost-ms 1500 --compare-log ../logs/mcu1.log
    uv run python -m noccarc_dashboard.capture --serial /dev/ttyUSB0 --duration 600

Renode: runs until Renode closes the port. Serial: until --duration or Ctrl-C.

Checks (each one can fail the run):
  - every frame decodes; sequence numbers missing = replaced by MCU1 + lost on the way, lost = 0;
  - events replayed between two snapshots give exactly the second one;
  - with --compare-log: the 1 s report and the snapshot of the same pass are identical (20 raw values,
    synced, health, online, own state, links), every link, restart, own-state and owner-offline
    change MCU1 logged has its event within 2 ms, and the other way round, however short the state,
    and MCU1 counted no change its change counts missed (ev_unflagged=0). MCU1's log starts again at
    t=0 after a reset, so it is split at each "MCU1 boot" line and matched to the link's MCU1 sessions;
  - with --expect-restarts / --expect-lost: how often MCU1 restarted and the link went LOST.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import threading
from collections import Counter
from pathlib import Path

from . import framing, monitor_pb2
from .events import describe, members, replay_check
from .link import DEFAULT_LOST_MS, LINK_BAUD, LinkCore, SerialSource, TcpSource, run
from .uptime import format_uptime


def raw_bits(p: monitor_pb2.Parameter) -> int:
    kind = p.WhichOneof("value")
    if kind == "f32":
        return struct.unpack("<I", struct.pack("<f", p.f32))[0]
    if kind == "i32":
        return p.i32 & 0xFFFFFFFF
    if kind == "u32":
        return p.u32
    return 1 if p.flag else 0


def _log_sessions(path: Path) -> list[list[str]]:
    """MCU1's log split at every "MCU1 boot" line: one list of lines per MCU1 boot."""
    sessions: list[list[str]] = []
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("MCU1 boot") or not sessions:
            sessions.append([])
        sessions[-1].append(line)
    return sessions


def read_log(path: Path) -> list[dict[int, dict]]:
    """Per MCU1 boot, its 1 s reports: time -> raw values, synced count, own state, links, health."""
    result = []
    for lines in _log_sessions(path):
        reports: dict[int, dict] = {}
        t = None
        for line in lines:
            if m := re.match(r"MCU1 t=(\d+) tx ", line):
                t = int(m.group(1))
                reports[t] = {}
            elif t is not None and (m := re.match(r"MCU1 db synced=(\d+)", line)):
                reports[t]["synced"] = int(m.group(1))
            elif t is not None and line.startswith("MCU1 val "):
                reports[t]["values"] = {int(k): int(v) for k, v in (kv.split("=") for kv in line.split()[2:])}
            elif t is not None and (m := re.match(r"MCU1 diag self=(\w+)(.*)", line)):
                reports[t]["self"] = m.group(1)
                reports[t]["links"] = {int(n): link for n, link in re.findall(r" n(\d)=([A-Z_]+)/", m.group(2))}
            elif t is not None and (m := re.match(r"MCU1 health=(\d+) online=(\d+)/", line)):
                reports[t]["health"] = int(m.group(1))
                reports[t]["online"] = int(m.group(2))
            elif t is not None and (m := re.match(r"MCU1 link (.*)", line)):
                reports[t]["link"] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", m.group(1))}
        result.append(reports)
    return result


_LOG_CHANGE = re.compile(r"MCU1 t=(\d+) (?:diag (n\d|self) ([A-Z_]+)|db OWNER_(OFFLINE|ONLINE)((?: \d+)+))")
Item = tuple[int, tuple]  # (time ms, what changed)


def log_changes(path: Path) -> list[list[Item]]:
    """Per MCU1 boot, its log lines for link, restart, own-state and owner-offline changes, one item
    per node or parameter."""
    result = []
    for lines in _log_sessions(path):
        items: list[Item] = []
        for line in lines:
            if not (m := _LOG_CHANGE.match(line)):
                continue
            t = int(m.group(1))
            if m.group(2) == "self":
                items.append((t, ("own", m.group(3))))
            elif m.group(2):
                node, word = int(m.group(2)[1:]), m.group(3)
                items.append((t, ("restart", node) if word == "RESTART" else ("link", node, word)))
            else:
                items += [(t, ("owner", m.group(4), int(p))) for p in m.group(5).split()]
        result.append(items)
    return result


def event_changes(events: list[monitor_pb2.Event]) -> list[Item]:
    """The same kinds of change, from the EVENT frames."""
    items: list[Item] = []
    for e in events:
        kind = e.WhichOneof("body")
        if kind == "own_state":
            items.append((e.time_ms, ("own", monitor_pb2.NodeState.Name(e.own_state))))
        elif kind == "node":
            restarted = e.node.WhichOneof("change") == "restarted"
            link = monitor_pb2.LinkState.Name(e.node.link)
            items += [(e.time_ms, ("restart", n) if restarted else ("link", n, link)) for n in members(e.node.nodes)]
        elif kind == "param" and e.param.change in (monitor_pb2.OWNER_WENT_OFFLINE, monitor_pb2.OWNER_BACK):
            word = "OFFLINE" if e.param.change == monitor_pb2.OWNER_WENT_OFFLINE else "ONLINE"
            items += [(e.time_ms, ("owner", word, p)) for p in members(e.param.params)]
    return items


def match_changes(logged: list[Item], sent: list[Item], start: int, end: int,
                  window: int = 2) -> tuple[list[str], int]:
    """Each change MCU1 logged between start and end must have its event at most window ms
    later, and each event a logged change at most window ms earlier. Every change counts, however
    short (events option b). MCU1 makes the event in the same main-loop pass as the change, and
    its longest pass is about 1.1 ms (docs/sched_design.md), so in whole ms the event time is at
    most 2 ms after the log line's. Returns (problems, largest delay seen in ms)."""
    problems, worst = [], 0
    for t, what in logged:
        if not start <= t <= end - window:
            continue
        delays = [te - t for te, w in sent if w == what and t <= te <= t + window]
        if delays:
            worst = max(worst, min(delays))
        else:
            problems.append(f"logged at t={t}, no event: {what}")
    for te, what in sent:
        if not any(w == what and t <= te <= t + window for t, w in logged):
            problems.append(f"event at t={te}, not in the log: {what}")
    return problems, worst


def compare_reports(snaps: list, reports: dict[int, dict]) -> tuple[int, list[str]]:
    """Each snapshot against the 1 s report of the same pass. Returns (compared, mismatch lines)."""
    compared, mismatches = 0, []
    for f in snaps:
        s = f.message
        # The report runs first in the same pass; the snapshot may read the clock a tick later.
        rep = next((reports[t] for t in range(s.uptime_ms, s.uptime_ms - 5, -1) if t in reports), None)
        if not rep or "values" not in rep:
            continue
        compared += 1
        problems = [f"P{p.id} {raw_bits(p)} vs {rep['values'][p.id]}" for p in s.params if raw_bits(p) != rep["values"][p.id]]
        if s.synced_count != rep["synced"]:
            problems.append(f"synced {s.synced_count} vs {rep['synced']}")
        if (s.health_percent, s.online_count) != (rep.get("health"), rep.get("online")):
            problems.append(f"health/online {s.health_percent}/{s.online_count} vs {rep.get('health')}/{rep.get('online')}")
        if monitor_pb2.NodeState.Name(s.nodes[0].reported_state) != rep["self"]:
            problems.append(f"own state {s.nodes[0].reported_state} vs {rep['self']}")
        for n in s.nodes[1:]:
            if monitor_pb2.LinkState.Name(n.link) != rep["links"].get(n.node):
                problems.append(f"n{n.node} {monitor_pb2.LinkState.Name(n.link)} vs {rep['links'].get(n.node)}")
        if problems:
            mismatches.append(f"MISMATCH t={s.uptime_ms}: " + "; ".join(problems))
    return compared, mismatches


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=3456)
    ap.add_argument("--serial", help="serial port of the board's USB-UART adapter, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=LINK_BAUD)
    ap.add_argument("--lost-ms", type=int, default=DEFAULT_LOST_MS,
                    help=f"LOST after this long without a valid frame (default {DEFAULT_LOST_MS}; Renode runs: 1500)")
    ap.add_argument("--duration", type=float, help="stop after this many seconds (serial)")
    ap.add_argument("--connect-timeout", type=float, default=120.0)
    ap.add_argument("--compare-log", type=Path, help="MCU1's debug log, read after the capture ends")
    ap.add_argument("--expect-restarts", type=int, help="MCU1 restarts the link must see")
    ap.add_argument("--expect-lost", type=int, help="CONNECTED -> LOST transitions the link must see")
    args = ap.parse_args()

    source = SerialSource(args.serial, args.baud) if args.serial else TcpSource(args.host, args.port)
    core = LinkCore(args.lost_ms, keep_frames=True)
    stop = threading.Event()
    if args.duration:
        timer = threading.Timer(args.duration, stop.set)
        timer.daemon = True
        timer.start()
    try:
        run(source, core, stop=stop, until_closed=args.serial is None, connect_timeout=args.connect_timeout)
    except KeyboardInterrupt:
        pass
    source.close()

    t0 = core.log[0][1] if core.log else 0.0
    print(f"link (LOST after {args.lost_ms} ms without a valid frame):")
    for wall, mono, state, why in core.log:
        print(f"  {wall[11:]}  +{mono - t0:6.1f} s  {state:<12} {why}")

    frames = core.frames
    kinds = Counter(f.type for f in frames)
    snaps = [f for f in frames if f.type == framing.SNAPSHOT]
    print(f"frames {len(frames)}: snapshot {kinds[framing.SNAPSHOT]}, config {kinds[framing.CONFIG]}, "
          f"event {kinds[framing.EVENT]}; errors {dict(core.errors) or 0}; sequence numbers missing {core.missing} "
          f"(replaced by MCU1 {core.replaced}, lost on the way {core.lost})")
    for s in core.sessions:
        print(f"  MCU1 session {s.index}: from sequence {s.first_sequence}, {s.frames} frames, {s.configs} CONFIG, "
              f"missing {s.missing} = replaced {s.replaced} + lost {s.lost}")
    print(f"MCU1 restarts seen: {core.restarts}; link went LOST {core.lost_periods} times")
    if snaps:
        print(f"MCU1 uptime at the last snapshot: {format_uptime(core.uptime.total_ms)} "
              f"(count wrapped {core.uptime.wraps} times)")
        sizes = [f.wire_bytes for f in snaps]
        print(f"snapshot on the wire: min {min(sizes)}, mean {sum(sizes) / len(sizes):.0f}, max {max(sizes)} bytes")
    if core.config is not None:
        c = core.config
        print(f"config: protocol {c.protocol_version}, firmware {c.firmware}, {c.arbitration_bps} / {c.data_bps} bit/s, "
              f"BRS {c.brs}, {c.node_count} nodes, {c.param_count} parameters, names {c.params[0].name}..{c.params[-1].name}")

    events = [f.message for f in frames if f.type == framing.EVENT]
    if events:
        print(f"events ({len(events)}):")
        for e in events[:80]:
            print(f"  {describe(e)}")
    checked, replay_problems = replay_check(frames)
    for p in replay_problems[:20]:
        print(f"EVENT REPLAY: {p}")
    print(f"events replayed between snapshots: {checked} snapshots checked, {len(replay_problems)} differences")

    ok = bool(snaps) and not core.errors and core.lost == 0 and checked > 0 and not replay_problems
    if args.expect_restarts is not None:
        no_config = [s.index for s in core.sessions[1:] if s.configs == 0]
        good = core.restarts == args.expect_restarts and not no_config
        print(f"expected {args.expect_restarts} MCU1 restarts: saw {core.restarts}"
              + (f"; no CONFIG after restart in session {no_config}" if no_config else "") + (" OK" if good else " FAIL"))
        ok = ok and good
    if args.expect_lost is not None:
        good = core.lost_periods == args.expect_lost
        print(f"expected the link LOST {args.expect_lost} times: saw {core.lost_periods}" + (" OK" if good else " FAIL"))
        ok = ok and good

    if args.compare_log:
        log_reports, log_items = read_log(args.compare_log), log_changes(args.compare_log)
        offset = len(log_reports) - len(core.sessions)  # the capture holds the latest MCU1 boots
        if offset < 0 or not core.sessions:
            print(f"compare with {args.compare_log.name}: {len(log_reports)} boots in the log, "
                  f"{len(core.sessions)} sessions captured, cannot pair them")
            ok = False
        compared_total = mismatch_total = 0
        for s in core.sessions if offset >= 0 else []:
            mine = [f for f, k in zip(frames, core.frame_sessions) if k == s.index]
            s_snaps = [f for f in mine if f.type == framing.SNAPSHOT]
            s_events = [f.message for f in mine if f.type == framing.EVENT]
            compared, mismatches = compare_reports(s_snaps, log_reports[offset + s.index])
            for line in mismatches:
                print(line)
            compared_total += compared
            mismatch_total += len(mismatches)
            if s_snaps:
                start = min([f.message.uptime_ms for f in s_snaps] + [e.time_ms for e in s_events])
                end = s_snaps[-1].message.uptime_ms
                problems, worst = match_changes(log_items[offset + s.index], event_changes(s_events), start, end)
                for p in problems[:20]:
                    print(f"EVENT vs LOG (session {s.index}): {p}")
                print(f"session {s.index}: events vs log (links, restarts, own state, owner offline): "
                      f"{len(problems)} differences; event at most {worst} ms after its log line")
                ok = ok and not problems
            # MCU1's own guard: changes its look before a snapshot found that no change count
            # flagged (diag/ or params/ missed one). From the session's last 1 s report.
            links = [r["link"] for _, r in sorted(log_reports[offset + s.index].items()) if "link" in r]
            if links:
                last = links[-1]
                print(f"session {s.index}: MCU1 events made {last.get('ev')}, dropped {last.get('ev_drop')}, "
                      f"queue peak {last.get('ev_peak')}, changes no count flagged {last.get('ev_unflagged')}")
                ok = ok and last.get("ev_unflagged") == 0
            else:
                print(f"session {s.index}: no MCU1 link line in the log")
                ok = False
        print(f"compared with {args.compare_log.name}: {compared_total} snapshots, {mismatch_total} with differences")
        ok = ok and compared_total > 0 and mismatch_total == 0
    print("UART LINK PASS" if ok else "UART LINK FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
