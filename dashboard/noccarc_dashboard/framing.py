"""UART link framing (Design Document §8.2, §8.5).

On the wire every frame is COBS-encoded and ends with 0x00. Decoded, it is:
    [version << 4 | type][sequence, uint32 LE][Protobuf payload][CRC-16/CCITT-FALSE LE]
The CRC covers everything before it. The payload type comes from proto/monitor.proto.
Validation order (§8.5): COBS -> length -> CRC -> version -> known type -> Protobuf decode.
The sequence gap check belongs to the link, which remembers the previous frame.
"""

from __future__ import annotations

from dataclasses import dataclass

from google.protobuf.message import DecodeError, Message

from . import monitor_pb2

PROTOCOL_VERSION = 1
SNAPSHOT = 1
EVENT = 2
CONFIG = 3

MESSAGE_TYPES: dict[int, type[Message]] = {
    SNAPSHOT: monitor_pb2.Snapshot,
    EVENT: monitor_pb2.Event,
    CONFIG: monitor_pb2.Config,
}

HEADER_BYTES = 5
CRC_BYTES = 2


class FrameError(ValueError):
    """A frame that failed one of the checks; reason names which one."""

    def __init__(self, reason: str, detail: str = "") -> None:
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason = reason


@dataclass
class Frame:
    version: int
    type: int
    sequence: int
    message: Message
    wire_bytes: int  # COBS-encoded length plus the 0x00 terminator


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE, as util/crc16.c: polynomial 0x1021, init 0xFFFF, no reflection."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_decode(data: bytes) -> bytes:
    """Reverses monitor/cobs.c. Raises FrameError on a malformed group."""
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0:
            raise FrameError("cobs", "0x00 inside a frame")
        end = i + code - 1
        if end > len(data):
            raise FrameError("cobs", "group runs past the end")
        out += data[i:end]
        i = end
        if code < 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def parse_frame(encoded: bytes) -> Frame:
    """Decodes one frame given without its 0x00 terminator."""
    raw = cobs_decode(encoded)
    if len(raw) < HEADER_BYTES + CRC_BYTES:
        raise FrameError("length", f"{len(raw)} bytes")
    body, crc_bytes = raw[:-CRC_BYTES], raw[-CRC_BYTES:]
    if int.from_bytes(crc_bytes, "little") != crc16(body):
        raise FrameError("crc")
    version, frame_type = body[0] >> 4, body[0] & 0x0F
    if version != PROTOCOL_VERSION:
        raise FrameError("version", str(version))
    if frame_type not in MESSAGE_TYPES:
        raise FrameError("type", str(frame_type))
    message = MESSAGE_TYPES[frame_type]()
    try:
        message.ParseFromString(body[HEADER_BYTES:])
    except DecodeError as err:
        raise FrameError("protobuf", str(err)) from err
    sequence = int.from_bytes(body[1:HEADER_BYTES], "little")
    return Frame(version, frame_type, sequence, message, len(encoded) + 1)


def split_frames(buffer: bytes) -> tuple[list[bytes], bytes]:
    """Splits a byte stream at 0x00. Returns the complete frames and the unfinished rest."""
    *frames, rest = buffer.split(b"\x00")
    return [f for f in frames if f], rest
