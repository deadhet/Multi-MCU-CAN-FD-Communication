"""Python side of the UART link: COBS, CRC, frame checks, and the C -> Python cross-check.

build/host/monitor_frames.bin is written by the C test (firmware/tests/host/test_monitor.c) from
MCU1's real monitor code. Decoding it here proves the C encoder (nanopb) and the Python decoder
agree on every field. Run firmware/tests/host/run_host_tests.sh, which runs this test last.
"""

import math
import struct
from pathlib import Path

import pytest

from noccarc_dashboard import framing, monitor_pb2

FRAMES_FILE = Path(__file__).resolve().parents[2] / "build" / "host" / "monitor_frames.bin"


def test_crc16_check_value():
    assert framing.crc16(b"123456789") == 0x29B1  # CRC-16/CCITT-FALSE check value, as the firmware


@pytest.mark.parametrize(
    "encoded, decoded",
    [
        (b"\x01", b""),
        (b"\x01\x01", b"\x00"),
        (b"\x01\x01\x01", b"\x00\x00"),
        (b"\x03\x11\x22\x02\x33", b"\x11\x22\x00\x33"),
        (b"\x02\x11\x01\x01\x01", b"\x11\x00\x00\x00"),
    ],
)
def test_cobs_vectors(encoded, decoded):
    assert framing.cobs_decode(encoded) == decoded  # the same vectors as the C test


def test_cobs_full_group():
    data = bytes(1 + (i % 255) for i in range(254))
    assert framing.cobs_decode(b"\xff" + data + b"\x01") == data


def test_bad_frames_are_rejected():
    with pytest.raises(framing.FrameError, match="cobs"):
        framing.cobs_decode(b"\x05\x11")
    with pytest.raises(framing.FrameError, match="length"):
        framing.parse_frame(b"\x03\x11\x22")
    body = bytes([0x11, 1, 0, 0, 0])
    good = body + framing.crc16(body).to_bytes(2, "little")
    bad = good[:-1] + bytes([good[-1] ^ 1])
    from_raw = lambda raw: framing.parse_frame(_cobs_encode(raw))  # noqa: E731
    from_raw(good)
    with pytest.raises(framing.FrameError, match="crc"):
        from_raw(bad)
    wrong_version = bytes([0x21, 1, 0, 0, 0])
    with pytest.raises(framing.FrameError, match="version"):
        from_raw(wrong_version + framing.crc16(wrong_version).to_bytes(2, "little"))
    unknown_type = bytes([0x1F, 1, 0, 0, 0])
    with pytest.raises(framing.FrameError, match="type"):
        from_raw(unknown_type + framing.crc16(unknown_type).to_bytes(2, "little"))


def test_split_frames():
    frames, rest = framing.split_frames(b"\x02\x11\x00\x00\x03\x22\x33\x00\x01")
    assert frames == [b"\x02\x11", b"\x03\x22\x33"] and rest == b"\x01"


def _cobs_encode(data: bytes) -> bytes:
    out, block = bytearray(), bytearray()
    for byte in data:
        if byte == 0:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
        else:
            block.append(byte)
            if len(block) == 254:
                out += b"\xff" + block
                block = bytearray()
    return bytes(out + bytes([len(block) + 1]) + block)


def _f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


@pytest.fixture(scope="module")
def c_frames():
    if not FRAMES_FILE.exists():
        pytest.fail(f"{FRAMES_FILE} missing: run firmware/tests/host/run_host_tests.sh")
    frames, rest = framing.split_frames(FRAMES_FILE.read_bytes())
    assert rest == b""
    return [framing.parse_frame(f) for f in frames]


def test_c_snapshot_decodes_in_python(c_frames):
    snap_frame = c_frames[0]
    assert (snap_frame.type, snap_frame.sequence) == (framing.SNAPSHOT, 1)
    s = snap_frame.message
    assert (s.uptime_ms, len(s.nodes), len(s.params)) == (1005, 4, 20)
    assert (s.online_count, s.synced_count, s.frames_skipped) == (2, 12, 0)
    assert s.health_percent == 50  # the weaker of 2/4 and 12/20

    n = s.nodes
    assert (n[0].node, n[0].link, n[0].reported_state) == (1, monitor_pb2.SELF, monitor_pb2.OPERATIONAL)
    assert (n[0].synced_count, n[0].error_summary) == (12, 2)
    assert (n[1].link, n[1].heartbeat_age_ms, n[1].synced_count) == (monitor_pb2.ONLINE, 5, 20)
    assert (n[2].link, n[2].missed, n[2].restarts, n[2].recoveries) == (monitor_pb2.OFFLINE, 11, 1, 1)
    assert (n[2].error_summary, n[2].data_age_ms) == (7, 1210)
    assert (n[3].link, n[3].reported_state) == (monitor_pb2.UNKNOWN, monitor_pb2.BOOT)

    p = s.params
    assert p[0].WhichOneof("value") == "f32" and p[0].f32 == 1.5
    assert p[1].f32 == -2.25
    assert p[2].WhichOneof("value") == "i32" and p[2].i32 == -910
    assert p[3].WhichOneof("value") == "u32" and p[3].u32 == 4_000_000_000
    assert p[4].WhichOneof("value") == "flag" and p[4].flag is True
    assert (p[0].validity, p[0].freshness, p[0].age_ms, p[0].sequence, p[0].source) == (
        monitor_pb2.VALID, monitor_pb2.FRESH, 3, 7, 1)
    assert math.isnan(p[5].f32) and p[5].validity == monitor_pb2.INVALID
    assert p[10].f32 == _f32(296.716) and (p[10].sequence, p[10].source) == (1500, 3)
    assert (p[15].freshness, p[15].owner_offline, p[15].source_status, p[15].age_ms) == (
        monitor_pb2.STALE, True, monitor_pb2.SOURCE_STALE, 1205)
    assert (p[19].id, p[19].validity, p[19].WhichOneof("value"), p[19].flag) == (
        20, monitor_pb2.NEVER_RECEIVED, "flag", False)


def test_c_config_decodes_in_python(c_frames):
    cfg_frame = c_frames[1]
    assert (cfg_frame.type, cfg_frame.sequence) == (framing.CONFIG, 2)
    c = cfg_frame.message
    assert (c.protocol_version, c.firmware) == (1, "1.0-dev")
    assert (c.arbitration_bps, c.data_bps, c.brs) == (500_000, 2_000_000, True)
    assert (c.node_count, c.param_count, len(c.params)) == (4, 20, 20)
    assert (c.param_period_ms, c.heartbeat_period_ms, c.snapshot_period_ms) == (10, 100, 100)
    assert [i.name for i in c.params] == [f"Param {k}" for k in range(1, 21)]
    assert (c.params[10].owner, c.params[4].sample_period_ms, c.params[3].sample_period_ms) == (3, 1000, 500)


def test_newer_firmware_fields_are_ignored(c_frames):
    """Forward compatibility, one of the reasons for Protobuf: a field this dashboard does not
    know (here field 99, as a newer firmware might add) is skipped, and every known field still
    decodes."""
    original = c_frames[0].message
    payload = original.SerializeToString()
    unknown_field = bytes([0x98, 0x06, 0x2A])  # tag: field 99, varint; value 42
    newer = monitor_pb2.Snapshot()
    newer.ParseFromString(payload + unknown_field)
    assert newer.uptime_ms == original.uptime_ms and list(newer.params) == list(original.params)
    assert list(newer.nodes) == list(original.nodes) and newer.synced_count == original.synced_count
