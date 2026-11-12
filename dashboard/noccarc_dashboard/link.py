"""The UART link as the PC sees it (UART step 4): bytes in, frames out, the link state, MCU1's sessions.

Link states (Design Document §8.5):
    DISCONNECTED  no byte source: Renode's TCP port not open (yet, or any more), or the USB-UART adapter
                  unplugged. The reader tries again every second.
    LOST          the source is open but no valid frame came for lost_ms (default 300 ms: three snapshot
                  periods). Bytes that fail the frame checks do not count: a noisy link is not a working one.
    CONNECTED     a valid frame within the last lost_ms.

MCU1 sessions: a frame whose sequence number went back means MCU1 restarted (its first frame after a
reset is 1). Everything MCU1 counts from boot (the sequence, frames_skipped) starts again, so sequence
gaps are accounted per session: missing = replaced by MCU1 (growth of frames_skipped) + lost on the way.

Time is the PC's monotonic clock; the link log also stamps the PC's date and time. Renode runs slower
than real time (10 simulated seconds took 38 s on this PC), so a snapshot reaches the PC only every
~0.4 s there: Renode runs pass a larger lost_ms (renode/check_uart_link.sh: 1500 ms). The board uses
the default.

LinkCore has no threads and takes the time as an argument, so tests drive it with a fake clock.
Link runs a source and a LinkCore in a background thread for the dashboard.
"""

from __future__ import annotations

import contextlib
import socket
import threading
import time
from collections import Counter, deque
from dataclasses import dataclass
from datetime import datetime

from . import framing
from .uptime import UptimeTracker

CONNECTED = "CONNECTED"
LOST = "LOST"
DISCONNECTED = "DISCONNECTED"
DEFAULT_LOST_MS = 300
LINK_BAUD = 230_400  # Design Document §8.1, 8N1


def _went_back(new: int, old: int) -> bool:
    """The C signed-difference test: the sequence went back (or repeated)."""
    step = (new - old) % (1 << 32)
    return step == 0 or step >= (1 << 31)


@dataclass
class Session:
    """Frames from one MCU1 boot."""

    index: int
    first_sequence: int
    frames: int = 0
    configs: int = 0
    missing: int = 0                  # sequence numbers never received
    skipped_first: int | None = None  # frames_skipped in this session's first snapshot
    skipped_last: int | None = None

    @property
    def replaced(self) -> int:
        """Frames MCU1 made but never sent (replaced, or events refused), since the first snapshot."""
        return 0 if self.skipped_first is None else self.skipped_last - self.skipped_first

    @property
    def lost(self) -> int:
        return self.missing - self.replaced


class LinkCore:
    def __init__(self, lost_ms: int = DEFAULT_LOST_MS, keep_frames: bool = False, history: int = 500) -> None:
        self.lost_ms = lost_ms
        self.state = DISCONNECTED
        self.log: deque[tuple[str, float, str, str]] = deque(maxlen=history)  # (date-time, monotonic, state, why)
        self.errors: Counter[str] = Counter()  # frames that failed a check, by reason
        self.sessions: list[Session] = []
        self.uptime = UptimeTracker()
        self.snapshot = None        # latest monitor_pb2.Snapshot
        self.config = None          # latest monitor_pb2.Config
        self.events: deque = deque(maxlen=history)  # (PC date-time, monitor_pb2.Event)
        self.lost_periods = 0       # CONNECTED -> LOST transitions
        self.frames: list[framing.Frame] | None = [] if keep_frames else None
        self.frame_sessions: list[int] = []  # session index of each kept frame
        self._buffer = b""
        self._synced = False
        self._last_valid: float | None = None
        self._last_sequence: int | None = None

    # -- source events ---------------------------------------------------------------------------
    def opened(self, now: float) -> None:
        self._buffer, self._synced = b"", False
        self._set(LOST, now, "source open, no valid frame yet")

    def closed(self, now: float, why: str = "") -> None:
        self._buffer, self._synced = b"", False
        self._set(DISCONNECTED, now, why or "source closed")

    def feed(self, data: bytes, now: float) -> None:
        """Bytes as they arrive, in any pieces."""
        self._buffer += data
        if not self._synced:
            # The first piece, up to the first 0x00, is a whole frame if we connected before MCU1
            # sent it, or the tail of one if we joined mid-frame. Keep it if it decodes; drop it
            # silently if not (a joined-late tail is not a link error).
            if b"\x00" not in self._buffer:
                self._buffer = self._buffer[-4096:]
                self.tick(now)
                return
            first, self._buffer = self._buffer.split(b"\x00", 1)
            self._synced = True
            if first:
                with contextlib.suppress(framing.FrameError):
                    self._on_frame(framing.parse_frame(first), now)
        encoded, self._buffer = framing.split_frames(self._buffer)
        for enc in encoded:
            try:
                frame = framing.parse_frame(enc)
            except framing.FrameError as err:
                self.errors[err.reason] += 1
                continue
            self._on_frame(frame, now)
        self.tick(now)

    def tick(self, now: float) -> None:
        """Call regularly: CONNECTED becomes LOST after lost_ms without a valid frame."""
        if self.state == CONNECTED and (now - self._last_valid) * 1000.0 > self.lost_ms:
            self._set(LOST, now, f"no valid frame for {self.lost_ms} ms")

    # -- counts across sessions ------------------------------------------------------------------
    @property
    def restarts(self) -> int:
        return max(0, len(self.sessions) - 1)

    @property
    def missing(self) -> int:
        return sum(s.missing for s in self.sessions)

    @property
    def replaced(self) -> int:
        return sum(s.replaced for s in self.sessions)

    @property
    def lost(self) -> int:
        return sum(s.lost for s in self.sessions)

    # -- internals -------------------------------------------------------------------------------
    def _set(self, state: str, now: float, why: str) -> None:
        if state == self.state:
            return
        if self.state == CONNECTED and state == LOST:
            self.lost_periods += 1
        self.state = state
        self.log.append((datetime.now().isoformat(timespec="milliseconds"), now, state, why))

    def _on_frame(self, f: framing.Frame, now: float) -> None:
        if self._last_sequence is None or _went_back(f.sequence, self._last_sequence):
            self.sessions.append(Session(len(self.sessions), f.sequence))
            if len(self.sessions) > 1:  # logged whatever the state: a quick reset may never show LOST
                self.log.append((datetime.now().isoformat(timespec="milliseconds"), now, self.state,
                                 f"MCU1 restarted (sequence went back to {f.sequence})"))
        else:
            self.sessions[-1].missing += (f.sequence - self._last_sequence - 1) % (1 << 32)
        self._last_sequence = f.sequence
        s = self.sessions[-1]
        s.frames += 1
        if f.type == framing.SNAPSHOT:
            self.uptime.on_snapshot(f.sequence, f.message.uptime_ms)
            if s.skipped_first is None:
                s.skipped_first = f.message.frames_skipped
            s.skipped_last = f.message.frames_skipped
            self.snapshot = f.message
        else:
            self.uptime.on_frame(f.sequence)
            if f.type == framing.CONFIG:
                s.configs += 1
                self.config = f.message
            else:
                self.events.append((datetime.now().isoformat(timespec="milliseconds"), f.message))
        if self.frames is not None:
            self.frames.append(f)
            self.frame_sessions.append(s.index)
        self._last_valid = now
        self._set(CONNECTED, now, "valid frame")


# -- byte sources ---------------------------------------------------------------------------------
class TcpSource:
    """Renode's ServerSocketTerminal for MCU1's USART3 (renode/system.resc: port 3456)."""

    def __init__(self, host: str = "localhost", port: int = 3456) -> None:
        self.host, self.port = host, port
        self._sock: socket.socket | None = None

    def open(self) -> None:
        self._sock = socket.create_connection((self.host, self.port), timeout=1.0)
        self._sock.settimeout(0.05)

    def read(self) -> bytes:
        """b"" when nothing arrived for 50 ms; raises ConnectionError when the other side closed."""
        try:
            data = self._sock.recv(4096)
        except socket.timeout:
            return b""
        if not data:
            raise ConnectionError("closed by the other side")
        return data

    def close(self) -> None:
        if self._sock is not None:
            with contextlib.suppress(OSError):
                self._sock.close()
            self._sock = None


class SerialSource:
    """The real board: a 3.3 V USB-UART adapter on PD8, 230,400 baud, 8N1."""

    def __init__(self, port: str, baud: int = LINK_BAUD) -> None:
        self.port, self.baud = port, baud
        self._ser = None

    def open(self) -> None:
        import serial  # pyserial; imported here so TCP-only use never needs it

        self._ser = serial.Serial(self.port, self.baud, timeout=0.05)  # 8N1 is pyserial's default

    def read(self) -> bytes:
        return self._ser.read(4096)  # b"" on timeout; SerialException (an OSError) when unplugged

    def close(self) -> None:
        if self._ser is not None:
            with contextlib.suppress(OSError):
                self._ser.close()
            self._ser = None


# -- reading loop ---------------------------------------------------------------------------------
def run(source, core: LinkCore, *, stop: threading.Event | None = None, lock=None, retry_s: float = 1.0,
        until_closed: bool = False, connect_timeout: float | None = None, clock=time.monotonic) -> None:
    """Reads the source into core until stop is set. A closed source is reopened every retry_s.
    until_closed: return when the source closes after it was open (one recorded Renode run).
    connect_timeout: give up (raise) if the source never opens within this many seconds."""
    guard = lock if lock is not None else contextlib.nullcontext()
    start = clock()
    is_open = False
    while stop is None or not stop.is_set():
        if not is_open:
            try:
                source.open()
            except OSError:
                if connect_timeout is not None and clock() - start > connect_timeout:
                    raise
                time.sleep(retry_s)
                continue
            is_open = True
            with guard:
                core.opened(clock())
        try:
            data = source.read()
        except OSError as err:
            source.close()
            is_open = False
            with guard:
                core.closed(clock(), str(err))
            if until_closed:
                return
            continue
        with guard:
            if data:
                core.feed(data, clock())
            else:
                core.tick(clock())


class Link:
    """A source and a LinkCore in a background thread. view() gives a consistent copy for the GUI."""

    def __init__(self, source, lost_ms: int = DEFAULT_LOST_MS) -> None:
        self.source = source
        self.core = LinkCore(lost_ms)
        self.lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=run, args=(self.source, self.core),
                                        kwargs={"stop": self._stop, "lock": self.lock}, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        self.source.close()

    def view(self) -> dict:
        """Everything the dashboard shows, copied under the lock. Messages are replaced, never
        changed, when new frames arrive, so handing out the latest objects is safe."""
        with self.lock:
            c = self.core
            return {
                "state": c.state,
                "snapshot": c.snapshot,
                "config": c.config,
                "events": list(c.events),
                "log": list(c.log),
                "errors": dict(c.errors),
                "missing": c.missing,
                "replaced": c.replaced,
                "lost": c.lost,
                "restarts": c.restarts,
                "uptime_ms": c.uptime.total_ms,
            }
