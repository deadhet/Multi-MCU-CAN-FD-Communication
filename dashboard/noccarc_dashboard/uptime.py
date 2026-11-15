"""MCU1's uptime as the dashboard shows it: computed here, not on the MCU.

MCU1 sends its uptime as a uint32 count of milliseconds, which wraps to 0 after 2^32 ms
(49.7 days). The firmware never needs the total: it only subtracts nearby times, and that stays
right across the wrap. The dashboard needs the total, so it keeps it: it remembers the previous
snapshot and adds 2^32 every time the count wraps.

Wrap or restart? Both make the uptime go down. Rules, in order:
  1. The frame sequence went back: MCU1 restarted (its first frame after a reset is 1). This is
     comm/'s rule for heartbeats too: a lower uptime alone is not a restart.
  2. The uptime went down, the sequence went on: a wrap, if it fits the frames in between.
     MCU1 makes a snapshot every 100 ms, so each sequence step stands for at most 100 ms of its
     time. A real wrap moves the count forward (modulo 2^32) by about that much. A restart
     while the dashboard heard nothing (cable out, then more frames than before the reset)
     moves it by days: that is a restart.
The sequence, not the PC clock, measures the time between frames: Renode's clock does not run
at the PC's speed.

The date and time of day come from the PC: each frame is stamped when it arrives. The MCU has no
calendar.
"""

from __future__ import annotations

WRAP = 1 << 32
MS_PER_SEQUENCE_STEP = 100  # at most: a snapshot every 100 ms, CONFIG and events add more frames
SLACK_MS = 1000


def _forward(new: int, old: int) -> int:
    """How far new is ahead of old on a 32-bit counter (wraps like the C unsigned subtraction)."""
    return (new - old) % WRAP


def _went_back(new: int, old: int) -> bool:
    """The C signed-difference test: (int32_t)(new - old) < 0, or the same number again."""
    step = _forward(new, old)
    return step == 0 or step >= WRAP // 2


class UptimeTracker:
    """Feed every frame's sequence, and the uptime of every snapshot."""

    def __init__(self) -> None:
        self.wraps = 0
        self.restarts = 0
        self._last_sequence: int | None = None
        self._last_uptime: int | None = None
        self._sequence_at_last_uptime: int | None = None

    def on_frame(self, sequence: int) -> None:
        """Every frame, any type: a sequence that went back means MCU1 restarted (rule 1)."""
        if self._last_sequence is not None and _went_back(sequence, self._last_sequence):
            self.restarts += 1
            self.wraps = 0
            self._last_uptime = None
        self._last_sequence = sequence

    def on_snapshot(self, sequence: int, uptime_ms: int) -> int:
        """A snapshot's uptime field. Returns MCU1's total uptime in ms since its last start."""
        self.on_frame(sequence)
        if self._last_uptime is not None and uptime_ms < self._last_uptime:
            steps = _forward(sequence, self._sequence_at_last_uptime)
            if _forward(uptime_ms, self._last_uptime) <= steps * MS_PER_SEQUENCE_STEP + SLACK_MS:
                self.wraps += 1  # rule 2: the count went over the top
            else:
                self.restarts += 1  # too far for the frames in between: a restart we did not hear
                self.wraps = 0
        self._last_uptime = uptime_ms
        self._sequence_at_last_uptime = sequence
        return self.total_ms

    @property
    def total_ms(self) -> int:
        return 0 if self._last_uptime is None else self.wraps * WRAP + self._last_uptime


def format_uptime(ms: int) -> str:
    """'52 d 03:14:07.250' style, for the dashboard."""
    seconds, ms = divmod(ms, 1000)
    minutes, seconds = divmod(seconds, 60)
    hours, minutes = divmod(minutes, 60)
    days, hours = divmod(hours, 24)
    return f"{days} d {hours:02}:{minutes:02}:{seconds:02}.{ms:03}"
