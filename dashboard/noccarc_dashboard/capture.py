"""Captures MCU1's UART link from Renode (TCP) or a serial port, decodes every frame, and
optionally compares the snapshots with MCU1's debug log.

    uv run python -m noccarc_dashboard.capture --port 3456 --compare-log ../logs/mcu1.log

The debug report at t ms and the snapshot right after it run in the same main-loop pass on
MCU1 (both 5 ms after a send), so all 20 values (raw 4 bytes), the synced count, the health and
online count, MCU1's own state and every node's link state must be identical in both.
"""

from __future__ import annotations

import argparse
import re
import socket
import struct
import sys
import time
from collections import Counter
from pathlib import Path

from . import framing, monitor_pb2
from .events import describe, members, replay_check
from .uptime import UptimeTracker, format_uptime


def connect(host: str, port: int, timeout_s: float) -> socket.socket:
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            return socket.create_connection((host, port), timeout=2)
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.2)


def raw_bits(p: monitor_pb2.Parameter) -> int:
    kind = p.WhichOneof("value")
    if kind == "f32":
        return struct.unpack("<I", struct.pack("<f", p.f32))[0]
    if kind == "i32":
        return p.i32 & 0xFFFFFFFF
    if kind == "u32":
        return p.u32
    return 1 if p.flag else 0


def read_log(path: Path) -> dict[int, dict]:
    """MCU1's 1 s reports: time -> raw values, synced count, own state, link per node."""
    reports: dict[int, dict] = {}
    t = None
    for line in path.read_text(errors="replace").splitlines():
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
    return reports


_LOG_CHANGE = re.compile(r"MCU1 t=(\d+) (?:diag (n\d|self) ([A-Z_]+)|db OWNER_(OFFLINE|ONLINE)((?: \d+)+))")
Item = tuple[int, tuple]  # (time ms, what changed)


def log_changes(path: Path) -> list[Item]:
    """MCU1's own log lines for link, restart, own-state and owner-offline changes, one item
    per node or parameter."""
    items: list[Item] = []
    for line in path.read_text(errors="replace").splitlines():
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
    return items


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


def _subject(what: tuple) -> tuple:
    """What changed, without the new value: ("link", 3, "LATE") -> ("link", 3)."""
    return what[:2] if what[0] in ("link", "owner") else what[:1] if what[0] == "own" else what


def match_changes(logged: list[Item], sent: list[Item], start: int, end: int, window: int = 10,
                  step: int = 5) -> tuple[list[str], list[str]]:
    """Each change MCU1 logged between start and end must have its event at most window ms
    later, and each event a logged change at most window ms earlier. A logged state that was
    replaced by the next one within one event step (5 ms) can be skipped by design: MCU1 looks
    every 5 ms and reports what it sees. Returns (problems, skipped by design)."""
    problems, skipped = [], []
    for t, what in logged:
        if start <= t <= end - window and not any(w == what and t <= te <= t + window for te, w in sent):
            if any(_subject(w) == _subject(what) and t <= t2 <= t + step and w != what for t2, w in logged):
                skipped.append(f"t={t} {what}, replaced within {step} ms")
            else:
                problems.append(f"logged at t={t}, no event: {what}")
    for te, what in sent:
        if not any(w == what and t <= te <= t + window for t, w in logged):
            problems.append(f"event at t={te}, not in the log: {what}")
    return problems, skipped


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=3456)
    ap.add_argument("--connect-timeout", type=float, default=120.0)
    ap.add_argument("--compare-log", type=Path, help="MCU1's debug log, read after the link closes")
    args = ap.parse_args()

    sock = connect(args.host, args.port, args.connect_timeout)
    sock.settimeout(None)
    buffer, synced_to_frames = b"", False
    frames: list[framing.Frame] = []
    errors: Counter[str] = Counter()
    missing = 0  # sequence numbers never received
    last_seq = None
    uptime = UptimeTracker()
    while chunk := sock.recv(4096):
        buffer += chunk
        if not synced_to_frames:  # joined mid-stream: drop the partial frame before the first 0x00
            if b"\x00" not in buffer:
                continue
            buffer = buffer.split(b"\x00", 1)[1]
            synced_to_frames = True
        encoded, buffer = framing.split_frames(buffer)
        for enc in encoded:
            try:
                frame = framing.parse_frame(enc)
            except framing.FrameError as err:
                errors[err.reason] += 1
                continue
            if last_seq is not None:
                missing += (frame.sequence - last_seq - 1) % (1 << 32)
            last_seq = frame.sequence
            if frame.type == framing.SNAPSHOT:
                uptime.on_snapshot(frame.sequence, frame.message.uptime_ms)
            else:
                uptime.on_frame(frame.sequence)
            frames.append(frame)

    kinds = Counter(f.type for f in frames)
    snaps = [f for f in frames if f.type == framing.SNAPSHOT]
    sizes = [f.wire_bytes for f in snaps]
    # A missing sequence number is a frame MCU1 replaced with a newer one while the link was busy
    # (counted in frames_skipped, §8.4) or one lost on the way. Only the second is a fault.
    replaced = snaps[-1].message.frames_skipped - snaps[0].message.frames_skipped if snaps else 0
    lost = missing - replaced
    print(f"frames {len(frames)}: snapshot {kinds[framing.SNAPSHOT]}, config {kinds[framing.CONFIG]}, "
          f"event {kinds[framing.EVENT]}; errors {dict(errors) or 0}; sequence numbers missing {missing} "
          f"(replaced by MCU1 {replaced}, lost on the way {lost})")
    if snaps:
        print(f"MCU1 uptime at the last snapshot: {format_uptime(uptime.total_ms)} "
              f"(count wrapped {uptime.wraps} times, restarts seen {uptime.restarts})")
    if sizes:
        print(f"snapshot on the wire: min {min(sizes)}, mean {sum(sizes) / len(sizes):.0f}, max {max(sizes)} bytes")
    configs = [f.message for f in frames if f.type == framing.CONFIG]
    if configs:
        c = configs[-1]
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

    ok = bool(snaps) and not errors and lost == 0 and checked > 0 and not replay_problems
    if args.compare_log and uptime.restarts == 0 and snaps:
        start = min([s.message.uptime_ms for s in snaps] + [e.time_ms for e in events])
        end = snaps[-1].message.uptime_ms
        change_problems, by_design = match_changes(log_changes(args.compare_log), event_changes(events), start, end)
        for p in change_problems[:20]:
            print(f"EVENT vs LOG: {p}")
        for p in by_design:
            print(f"  not reported by design (shorter than one 5 ms look): {p}")
        print(f"events vs {args.compare_log.name} (links, restarts, own state, owner offline): "
              f"{len(change_problems)} differences, {len(by_design)} short states not reported by design")
        ok = ok and not change_problems
    elif args.compare_log:
        print("events vs log: skipped (MCU1 restarted during the capture; its log starts again at t=0)")
    if args.compare_log:
        reports = read_log(args.compare_log)
        compared = mismatches = 0
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
                mismatches += 1
                print(f"MISMATCH t={s.uptime_ms}: " + "; ".join(problems))
        print(f"compared with {args.compare_log.name}: {compared} snapshots, {mismatches} with differences")
        ok = ok and compared > 0 and mismatches == 0
    print("UART LINK PASS" if ok else "UART LINK FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
