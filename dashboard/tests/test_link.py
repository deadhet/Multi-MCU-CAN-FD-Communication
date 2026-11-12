"""The PC's view of the UART link (noccarc_dashboard/link.py): link states, garbage, pieces, gaps,
MCU1 restarts, reconnecting. Time is a number passed in, so nothing here waits."""

import threading

from noccarc_dashboard import framing
from noccarc_dashboard import monitor_pb2 as pb
from noccarc_dashboard.link import CONNECTED, DISCONNECTED, LOST, Link, LinkCore, run


def snap(seq, uptime, skipped=0):
    return framing.encode_frame(framing.SNAPSHOT, seq, pb.Snapshot(uptime_ms=uptime, frames_skipped=skipped))


def cfg(seq):
    return framing.encode_frame(framing.CONFIG, seq, pb.Config(protocol_version=1))


def joined(lost_ms=300):
    """A core whose source just opened at t=0 and already passed its first 0x00."""
    core = LinkCore(lost_ms)
    core.opened(0.0)
    core.feed(b"\x00", 0.0)
    return core


def test_states_and_the_300_ms_limit():
    core = LinkCore()
    assert core.state == DISCONNECTED
    core.opened(0.0)
    assert core.state == LOST                       # open, but no valid frame yet
    core.feed(b"\x00" + snap(1, 5), 0.0)
    assert core.state == CONNECTED
    core.tick(0.300)
    assert core.state == CONNECTED                  # 300 ms: still within the limit
    core.tick(0.301)
    assert core.state == LOST and core.lost_periods == 1
    core.feed(snap(2, 105), 0.5)
    assert core.state == CONNECTED
    core.closed(0.6, "adapter unplugged")
    assert core.state == DISCONNECTED
    assert [entry[2] for entry in core.log] == [LOST, CONNECTED, LOST, CONNECTED, DISCONNECTED]


def test_a_larger_limit_for_renode():
    core = joined(lost_ms=1500)
    core.feed(snap(1, 5), 0.0)
    core.tick(1.0)
    assert core.state == CONNECTED
    core.tick(1.6)
    assert core.state == LOST


def test_garbage_does_not_keep_the_link_alive():
    core = joined()
    core.feed(snap(1, 5), 0.0)
    raw = bytearray(framing.cobs_decode(snap(2, 105)[:-1]))
    raw[6] ^= 0x40                                  # damage the payload, re-frame it: only the CRC fails
    core.feed(framing.cobs_encode(bytes(raw)) + b"\x00", 0.2)
    assert core.errors == {"crc": 1}
    core.tick(0.31)
    assert core.state == LOST                       # 310 ms since the last *valid* frame


def test_frames_in_any_pieces():
    core = joined()
    for b in snap(1, 5):
        core.feed(bytes([b]), 0.0)
    assert core.snapshot.uptime_ms == 5 and len(core.sessions[0:1]) == 1 and core.sessions[0].frames == 1


def test_joining_mid_frame_drops_only_the_partial_one():
    core = LinkCore()
    core.opened(0.0)
    core.feed(snap(7, 700)[10:] + snap(8, 800), 0.0)  # the tail of frame 7, then frame 8
    assert core.snapshot.uptime_ms == 800 and not core.errors


def test_connected_before_the_first_byte_keeps_the_first_frame():
    core = LinkCore()
    core.opened(0.0)
    core.feed(cfg(1) + snap(2, 5), 0.0)             # MCU1's very first frames, nothing before them
    assert core.sessions[0].first_sequence == 1 and core.sessions[0].configs == 1 and not core.errors


def test_gaps_replaced_or_lost():
    core = joined()
    core.feed(snap(1, 5, skipped=0) + cfg(2), 0.0)
    core.feed(snap(5, 105, skipped=2), 0.1)         # 3 and 4 missing; MCU1 says it replaced 2
    assert (core.missing, core.replaced, core.lost) == (2, 2, 0)
    core.feed(snap(8, 205, skipped=2), 0.2)         # 6 and 7 missing, nothing replaced: lost
    assert (core.missing, core.replaced, core.lost) == (4, 2, 2)


def test_mcu1_restart_starts_a_new_session():
    core = joined()
    core.feed(snap(100, 9000, skipped=3) + snap(101, 9100, skipped=3), 0.0)
    core.feed(cfg(1) + snap(2, 5, skipped=0), 0.1)  # reset: CONFIG first, sequence 1
    assert core.restarts == 1 and len(core.sessions) == 2
    assert core.sessions[1].configs == 1 and core.sessions[1].first_sequence == 1
    assert core.lost == 0                           # frames_skipped counted per session
    assert core.uptime.total_ms == 5
    assert any("MCU1 restarted" in entry[3] for entry in core.log)


class FakeSource:
    """Fails to open `fail_opens` times, then hands out the chunks, then reports the other side closed."""

    def __init__(self, chunks, fail_opens=0):
        self.chunks, self.fail_opens, self.opens = list(chunks), fail_opens, 0

    def open(self):
        self.opens += 1
        if self.opens <= self.fail_opens:
            raise ConnectionRefusedError("not listening yet")

    def read(self):
        if not self.chunks:
            raise ConnectionError("closed by the other side")
        return self.chunks.pop(0)

    def close(self):
        pass


def test_run_retries_until_the_source_opens_and_stops_when_it_closes():
    source = FakeSource([b"\x00" + snap(1, 5), b"", snap(2, 105)], fail_opens=2)
    core = LinkCore()
    run(source, core, retry_s=0.0, until_closed=True)
    assert source.opens == 3
    assert [entry[2] for entry in core.log] == [LOST, CONNECTED, DISCONNECTED]
    assert core.snapshot.uptime_ms == 105


def test_link_thread_and_view():
    ready = threading.Event()

    class SlowSource(FakeSource):
        def read(self):
            if not self.chunks:
                ready.set()
                return b""                          # stays open, nothing more
            return self.chunks.pop(0)

    link = Link(SlowSource([b"\x00" + snap(1, 5)]), lost_ms=60_000)
    link.start()
    assert ready.wait(2.0)
    view = link.view()
    link.stop()
    assert view["state"] == CONNECTED and view["snapshot"].uptime_ms == 5 and view["lost"] == 0
