"""Event replay against snapshots, and events against MCU1's log (noccarc_dashboard/events.py,
capture.py)."""

from noccarc_dashboard import framing
from noccarc_dashboard import monitor_pb2 as pb
from noccarc_dashboard.capture import match_changes
from noccarc_dashboard.events import members, replay_check


def snap(seq, t, links=None, own=pb.OPERATIONAL, params=None, skipped=0):
    """A snapshot: peers ONLINE and parameters VALID, FRESH unless given."""
    s = pb.Snapshot(uptime_ms=t, frames_skipped=skipped)
    s.nodes.add(node=1, link=pb.SELF, reported_state=own)
    for n, link in ({2: pb.ONLINE, 3: pb.ONLINE, 4: pb.ONLINE} | (links or {})).items():
        s.nodes.add(node=n, link=link)
    for pid in range(1, 21):
        validity, freshness, offline = (params or {}).get(pid, (pb.VALID, pb.FRESH, False))
        s.params.add(id=pid, validity=validity, freshness=freshness, owner_offline=offline)
    return framing.Frame(1, framing.SNAPSHOT, seq, s, 0)


def link_event(seq, t, nodes, link):
    return framing.Frame(1, framing.EVENT, seq, pb.Event(time_ms=t, node=pb.NodeEvent(nodes=nodes, link=link)), 0)


def param_event(seq, t, change, params):
    return framing.Frame(1, framing.EVENT, seq, pb.Event(time_ms=t, param=pb.ParamEvent(change=change, params=params)), 0)


STALE_OFFLINE = (pb.VALID, pb.STALE, True)
P11_15 = 0b11111 << 10


def test_members():
    assert members(0b1010) == [2, 4] and members(P11_15) == [11, 12, 13, 14, 15]


def test_events_turn_one_snapshot_into_the_next():
    frames = [
        snap(1, 5000),
        link_event(2, 5051, 0b100, pb.LATE),
        param_event(3, 5030, pb.BECAME_STALE, P11_15),
        link_event(4, 5251, 0b100, pb.OFFLINE),
        param_event(5, 5251, pb.OWNER_WENT_OFFLINE, P11_15),
        snap(6, 5300, links={3: pb.OFFLINE}, params={p: STALE_OFFLINE for p in range(11, 16)}),
    ]
    assert replay_check(frames) == (1, [])


def test_a_missing_event_is_found():
    frames = [
        snap(1, 5000),
        link_event(2, 5251, 0b100, pb.OFFLINE),
        snap(3, 5300, links={3: pb.OFFLINE}, params={p: STALE_OFFLINE for p in range(11, 16)}),
    ]
    checked, problems = replay_check(frames)
    assert checked == 1 and len(problems) == 10  # P11-P15: fresh and owner_offline
    assert "P11 fresh is False, events say True" in problems[0]


def test_the_events_before_a_snapshot_must_explain_it():
    """MCU1 looks once more before every snapshot, so the event comes first, even in the same ms."""
    frames = [
        snap(1, 3000),
        link_event(2, 3101, 0b100, pb.OFFLINE),
        snap(3, 3101, links={3: pb.OFFLINE}),
    ]
    assert replay_check(frames) == (1, [])
    after = [frames[0], frames[2], frames[1]]  # the event after its snapshot: no allowance any more
    assert replay_check(after)[1] == ["snapshot t=3101: n3 is 3, events say 1"]


def test_no_check_across_skipped_frames_or_a_restart():
    skipped = [snap(1, 1000), snap(5, 1100, links={2: pb.OFFLINE}, skipped=3)]
    assert replay_check(skipped) == (0, [])
    restart = [snap(50, 1000), snap(1, 5, links={2: pb.UNKNOWN})]  # sequence went back
    assert replay_check(restart) == (0, [])


def test_events_against_the_log():
    logged = [(5051, ("link", 3, "LATE")), (5251, ("link", 3, "OFFLINE")),
              (5251, ("owner", "OFFLINE", 11)), (90, ("link", 2, "ONLINE"))]
    sent = [(5053, ("link", 3, "LATE")), (5251, ("link", 3, "OFFLINE")), (5251, ("owner", "OFFLINE", 11))]
    assert match_changes(logged, sent, start=100, end=9000) == ([], 2)  # t=90: before the capture began
    late = [(5054, ("link", 3, "LATE"))] + sent[1:]
    assert len(match_changes(logged, late, start=100, end=9000)[0]) == 2  # 3 ms: missed, and unexplained
    extra = sent + [(6000, ("restart", 3))]
    assert match_changes(logged, extra, start=100, end=9000)[0] == ["event at t=6000, not in the log: ('restart', 3)"]


def test_a_short_state_must_have_its_event():
    """S2, before events option b: MCU1 RESYNCING from 100 to 105 at boot, between two 5 ms looks.
    Now every change has its event, however short."""
    logged = [(100, ("own", "RESYNCING")), (105, ("own", "OPERATIONAL"))]
    sent = [(105, ("own", "OPERATIONAL"))]
    assert match_changes(logged, sent, start=0, end=9000) == (["logged at t=100, no event: ('own', 'RESYNCING')"], 0)
    both = [(100, ("own", "RESYNCING"))] + sent
    assert match_changes(logged, both, start=0, end=9000) == ([], 0)
