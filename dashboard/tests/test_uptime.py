"""MCU1's uptime across the 49.7-day wrap of its uint32 ms count (noccarc_dashboard/uptime.py)."""

from noccarc_dashboard.uptime import WRAP, UptimeTracker, format_uptime

TOP = WRAP - 1  # 4 294 967 295 ms: the last value before the count wraps to 0


def test_normal_counting():
    t = UptimeTracker()
    assert t.on_snapshot(1, 5) == 5
    assert t.on_snapshot(2, 105) == 105
    assert (t.wraps, t.restarts) == (0, 0)


def test_wrap_adds_2_to_the_32():
    t = UptimeTracker()
    t.on_snapshot(1000, TOP - 95)
    assert t.on_snapshot(1001, 5) == WRAP + 5  # 100 ms later, past the top
    assert t.on_snapshot(1002, 105) == WRAP + 105
    assert (t.wraps, t.restarts) == (1, 0)


def test_two_wraps():
    t = UptimeTracker()
    t.on_snapshot(1, TOP - 50)
    t.on_snapshot(2, 50)
    t.on_snapshot(3, TOP - 50)  # (as if 49.7 days passed between frames 2 and 3: only the count matters here)
    assert t.on_snapshot(4, 50) == 2 * WRAP + 50


def test_wrap_with_frames_missing_in_between():
    t = UptimeTracker()
    t.on_snapshot(500, TOP - 1300)
    assert t.on_snapshot(530, 1700) == WRAP + 1700  # 30 frames lost, 3 s: still a wrap
    assert t.restarts == 0


def test_config_between_snapshots_does_not_confuse_it():
    t = UptimeTracker()
    t.on_snapshot(10, TOP - 95)
    t.on_frame(11)  # a CONFIG
    assert t.on_snapshot(12, 5) == WRAP + 5


def test_restart_sequence_back():
    t = UptimeTracker()
    t.on_snapshot(1000, TOP - 95)
    t.on_snapshot(1001, 5)  # wrap
    t.on_frame(1)  # MCU1 reset: CONFIG first, sequence 1
    assert t.on_snapshot(2, 5) == 5  # counts from the new start, the wrap is forgotten
    assert (t.wraps, t.restarts) == (0, 1)


def test_restart_not_heard_is_not_taken_for_a_wrap():
    t = UptimeTracker()
    t.on_snapshot(100, 2_000_000)  # 33 minutes up
    # Cable out; MCU1 resets; 600 frames later the dashboard hears it again, at 60 s uptime.
    assert t.on_snapshot(700, 60_000) == 60_000
    assert (t.wraps, t.restarts) == (0, 1)


def test_format():
    assert format_uptime(WRAP + 5) == "49 d 17:02:47.301"
    assert format_uptime(0) == "0 d 00:00:00.000"
