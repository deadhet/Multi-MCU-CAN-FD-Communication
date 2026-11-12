"""Captures MCU1's UART link from Renode (TCP) or a serial port, decodes every frame, and
optionally compares the snapshots with MCU1's debug log.

    uv run python -m noccarc_dashboard.capture --port 3456 --compare-log ../logs/mcu1.log

The debug report at t ms and the snapshot right after it run in the same main-loop pass on
MCU1 (both 5 ms after a send), so all 20 values (raw 4 bytes), the synced count, MCU1's own state and
every node's link state must be identical in both.
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
    return reports


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

    ok = bool(snaps) and not errors and lost == 0
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
