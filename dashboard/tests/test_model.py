"""What the dashboard shows (noccarc_dashboard/model.py): texts, levels, formatting, the header
in every link state. The page's rule: a healthy network shows no colour at all."""

import json
import math
from datetime import datetime

import pytest

from noccarc_dashboard import model
from noccarc_dashboard import monitor_pb2 as pb

NOW = datetime(2026, 11, 13, 2, 41, 14)


def snap(links=None, states=None, health=100, online=4, synced=20, missed=None):
    s = pb.Snapshot(uptime_ms=9905, health_percent=health, online_count=online, synced_count=synced)
    s.nodes.add(node=1, link=pb.SELF, reported_state=pb.OPERATIONAL)
    for n in (2, 3, 4):
        s.nodes.add(node=n, link=(links or {}).get(n, pb.ONLINE), reported_state=(states or {}).get(n, pb.OPERATIONAL),
                    heartbeat_age_ms=43, missed=(missed or {}).get(n, 0))
    for pid in range(1, 21):
        p = s.params.add(id=pid, source=(pid - 1) // 5 + 1, validity=pb.VALID, freshness=pb.FRESH, age_ms=5)
        slot = (pid - 1) % 5
        if slot < 2:
            p.f32 = 201.731
        elif slot == 2:
            p.i32 = -610
        elif slot == 3:
            p.u32 = 4_000_000_000
        else:
            p.flag = True
    return s


def config():
    c = pb.Config(protocol_version=1, firmware="1.0-dev", arbitration_bps=500_000, data_bps=2_000_000, brs=True,
                  param_period_ms=10, heartbeat_period_ms=100)
    for pid in range(1, 21):
        c.params.add(id=pid, name=f"Param {pid}", owner=(pid - 1) // 5 + 1)
    return c


def view(state="CONNECTED", snapshot=None, cfg=None, events=(), errors=None, lost=0, restarts=0, since=0.05):
    return {"state": state, "since_valid_s": since, "why": "", "frames": 121, "snapshot": snapshot, "config": cfg,
            "events": list(events), "log": [], "errors": errors or {}, "missing": lost, "replaced": 0, "lost": lost,
            "restarts": restarts, "uptime_ms": 9905}


def build(**kw):
    return model.build(view(**kw), "localhost:3456 (Renode)", NOW)


def levels(obj):
    """Every level in the model, wherever it is."""
    if isinstance(obj, dict):
        out = [obj["l"]] if "l" in obj else []
        return out + [x for v in obj.values() for x in levels(v)]
    if isinstance(obj, list):
        return [x for v in obj for x in levels(v)]
    return []


def node(m, n):
    return m["snapshot"]["nodes"][n - 1]


def text(c):
    return c if isinstance(c, str) else c["t"]


def level(c):
    return "" if isinstance(c, str) else c["l"]


def test_a_healthy_network_shows_no_colour():
    m = build(snapshot=snap(), cfg=config())
    assert m["link"] == "CONNECTED" and m["health_head"] == "100 % HEALTHY" and not m["stale"]
    assert set(levels(m)) <= {"", "dim"}
    assert m["source"] == "MCU1 over UART, localhost:3456 (Renode)"
    assert m["uptime"] == "0 d 00:00:09.905" and m["pc_time"] == "2026-11-13 02:41:14"
    json.dumps(m)  # the page gets it as JSON


def test_before_the_first_snapshot_nothing_is_shown():
    m = build(state="LOST", since=None)
    assert m["snapshot"] is None and m["uptime"] is None and m["config"] is None
    assert m["health_head"] == {"t": "NO DATA", "l": "dim"}
    assert m["link"] == {"t": "LOST: no valid frame yet", "l": "fault"} and not m["stale"]


def test_a_lost_link_keeps_the_last_data_marked_old():
    m = build(state="LOST", snapshot=snap(health=75), since=4.23)
    assert m["link"] == {"t": "LOST: no valid frame for 4.2 s", "l": "fault"}
    assert m["health_head"] == {"t": "NO DATA (last 75 %)", "l": "dim"} and m["stale"]
    assert m["snapshot"] is not None
    d = build(state="DISCONNECTED", snapshot=snap())
    assert d["link"]["l"] == "fault" and d["link"]["t"].startswith("DISCONNECTED") and d["stale"]


@pytest.mark.parametrize("pct,label,lv", [(100, "HEALTHY", ""), (99, "DEGRADED", "caution"),
                                         (1, "DEGRADED", "caution"), (0, "DOWN", "fault")])
def test_health_label(pct, label, lv):
    assert model.health_label(pct) == (label, lv)
    h = build(snapshot=snap(health=pct))["snapshot"]["health"]
    assert h["pct"] == pct and h["label"] == {"t": label, "l": lv}


def test_health_rule_shows_both_counts():
    h = build(snapshot=snap(health=50, online=2, synced=15))["snapshot"]["health"]
    assert "online (2/4 = 50 %)" in h["rule"] and "synchronized (15/20 = 75 %)" in h["rule"]
    assert (h["online"], h["synced"]) == ("2/4", "15/20")
    assert [len(g) for g in h["params"]] == [5, 5, 5, 5]


# MCU1's view of a peer: status, heartbeat, timeout, recovery (section 2).
@pytest.mark.parametrize("link,status_lv,heartbeat,timeout,recovery", [
    (pb.ONLINE, "", "Receiving", "None", "Normal (OPERATIONAL)"),
    (pb.LATE, "caution", "Late", "Heartbeat late", "Normal (OPERATIONAL)"),
    (pb.OFFLINE, "fault", "Not received", "Heartbeat and data", "Waiting for heartbeat"),
    (pb.RECOVERING, "info", "Receiving", "None", "Recovering: waiting for OPERATIONAL"),
    (pb.NO_HEARTBEAT, "caution", "Not received", "Heartbeat lost, data arriving", "Normal (OPERATIONAL)"),
    (pb.DATA_INVALID, "fault", "Receiving", "No valid data block", "Normal (OPERATIONAL)"),
    (pb.UNKNOWN, "info", "Not heard yet", "Not heard yet", "Not heard yet"),
])
def test_peer_columns(link, status_lv, heartbeat, timeout, recovery):
    row = node(build(snapshot=snap(links={3: link})), 3)
    assert row[0] == "MCU 3"
    assert text(row[1]) == pb.LinkState.Name(link) and level(row[1]) == status_lv
    assert (text(row[2]), text(row[5]), text(row[6])) == (heartbeat, timeout, recovery)
    assert text(row[3]) == ("never" if link == pb.UNKNOWN else "0.043 s")
    seg = build(snapshot=snap(links={3: link}))["snapshot"]["health"]["nodes"][2]
    assert seg["l"] == ("dim" if link == pb.UNKNOWN else status_lv)


def test_derived_columns_get_only_a_small_mark():
    row = node(build(snapshot=snap(links={3: pb.OFFLINE}, missed={3: 11})), 3)
    assert "m" not in row[1]                         # the state itself: marked in full
    assert all(row[k].get("m") for k in (2, 4, 5))   # heartbeat, missed, timeout: small marks


@pytest.mark.parametrize("state,txt,lv", [(pb.OPERATIONAL, "Normal (OPERATIONAL)", ""),
                                          (pb.RESYNCING, "Re-synchronizing (RESYNCING)", "info"),
                                          (pb.BOOT, "Booting (BOOT)", "info")])
def test_recovery_follows_the_reported_state(state, txt, lv):
    row = node(build(snapshot=snap(states={2: state})), 2)
    assert text(row[6]) == txt and level(row[6]) == lv


@pytest.mark.parametrize("missed,lv", [(0, ""), (1, "caution"), (2, "caution"), (3, "fault"), (11, "fault")])
def test_missed_heartbeats(missed, lv):
    c = node(build(snapshot=snap(missed={4: missed})), 4)[4]
    assert text(c) == str(missed) and level(c) == lv


def test_master_row_and_its_restarts_from_the_link():
    row = node(build(snapshot=snap(), restarts=1), 1)
    assert row[:2] == ["MCU 1 (master)", "This node"] and row[2] == {"t": "n/a", "l": "dim"}
    assert row[7] == "1"  # restarts the PC saw (sequence back), not a field MCU1 counts itself


def test_parameter_values_by_type():
    rows = build(snapshot=snap(), cfg=config())["snapshot"]["params"]
    assert [r[2] for r in rows[:5]] == ["201.731", "201.731", "-610", "4000000000", "true"]
    assert [r[3] for r in rows[:5]] == ["f32", "f32", "i32", "u32", "bool"]
    assert rows[0][:2] == ["P1", "Param 1"] and rows[19][4] == "MCU 4" and rows[0][7] == "5 ms"


@pytest.mark.parametrize("v,shown", [(0.0, "0.000"), (-0.0001, "0.000"), (12.3456, "12.346"),
                                     (1.5e7, "1.5e+07"), (math.inf, "inf"), (math.nan, "nan")])
def test_float_format(v, shown):
    assert model.fmt_float(v) == shown


def test_parameter_states():
    s = snap()
    s.params[0].validity = pb.NEVER_RECEIVED
    s.params[1].validity = pb.INVALID
    s.params[10].freshness = pb.STALE
    s.params[10].owner_offline = True
    s.params[11].age_ms = 12_345
    rows = build(snapshot=s)["snapshot"]["params"]
    assert rows[0][2] == rows[0][7] == {"t": "–", "l": "dim"} and text(rows[0][6]) == "NEVER RECEIVED"
    assert rows[1][6] == {"t": "INVALID", "l": "fault"} and rows[1][2] == "201.731"  # old value kept, flagged
    assert rows[10][4] == {"t": "MCU 3 offline", "l": "fault", "m": True}
    assert rows[10][5] == {"t": "STALE", "l": "caution"}
    assert rows[11][7] == "12.3 s"
    assert rows[0][1] == {"t": "–", "l": "dim"}  # no CONFIG yet: no names
    segs = [c["l"] for g in build(snapshot=s)["snapshot"]["health"]["params"] for c in g]
    assert segs[:2] == ["dim", "fault"] and segs[10] == "fault" and segs[2] == ""


def test_config_rows():
    rows = dict(model.config_rows(config()))
    assert rows["Arbitration bit rate"] == "500 kbit/s" and rows["Data bit rate"] == "2 Mbit/s"
    assert rows["Bit rate switch (BRS)"] == "on" and rows["Firmware, protocol"] == "1.0-dev, v1"
    assert [model.fmt_bps(b) for b in (125_000, 8_000_000, 1234)] == ["125 kbit/s", "8 Mbit/s", "1234 bit/s"]


def test_uart_rows():
    rows = dict(build(snapshot=snap())["uart"])
    assert rows["Lost on the way"] == "0" and rows["Frame errors"] == "0" and rows["MCU1 restarts seen"] == "0"
    bad = dict(build(snapshot=snap(), errors={"crc": 2, "cobs": 1}, lost=3, restarts=1)["uart"])
    assert bad["Frame errors"] == {"t": "3 (cobs 1, crc 2)", "l": "caution", "m": True}
    assert bad["Lost on the way"]["l"] == "caution" and bad["MCU1 restarts seen"]["l"] == "info"


def ev(t, session=0, **body):
    return ("2026-11-13T02:41:12.551", pb.Event(time_ms=t, **body), session)


def test_events_newest_first_with_words_and_levels():
    events = [
        ev(100, own_state=pb.RESYNCING),
        ev(5035, param=pb.ParamEvent(change=pb.BECAME_STALE, params=0b11 << 10)),
        ev(5355, node=pb.NodeEvent(nodes=0b100, link=pb.OFFLINE)),
        ev(9000, node=pb.NodeEvent(nodes=0b110, restarted=True)),
    ]
    rows = build(snapshot=snap(), events=events)["events"]
    assert rows[0] == ["9.000", "02:41:12.551", {"t": "MCU2 MCU3 RESTARTED", "l": "caution", "m": True}]
    assert rows[1][2] == {"t": "MCU3 OFFLINE", "l": "fault", "m": True}
    assert rows[2][2] == {"t": "P11 P12 became STALE", "l": "caution", "m": True}
    assert rows[3][2] == {"t": "MCU1 RESYNCING", "l": "info", "m": True}
    online = build(events=[ev(100, node=pb.NodeEvent(nodes=0b1110, link=pb.ONLINE))])["events"]
    assert online[0][2] == "MCU2 MCU3 MCU4 ONLINE"  # a normal change: plain text


def test_a_separator_where_mcu1_restarted():
    events = [ev(5252, 0, own_state=pb.OPERATIONAL), ev(100, 1, own_state=pb.RESYNCING),
              ev(105, 1, own_state=pb.OPERATIONAL)]
    rows = build(events=events)["events"]
    assert [r[0] for r in rows] == ["0.105", "0.100", "", "5.252"]
    assert rows[2][2]["t"].startswith("MCU1 restarted") and rows[2][2]["l"] == "info"


def test_event_log_is_capped():
    events = [ev(t, own_state=pb.OPERATIONAL) for t in range(model.EVENTS_SHOWN + 50)]
    rows = build(events=events)["events"]
    assert len(rows) == model.EVENTS_SHOWN and rows[0][0] == f"{(model.EVENTS_SHOWN + 49) / 1000:.3f}"


def test_text_from_the_device_is_passed_as_text():
    c = config()
    c.params[0].name = "<img src=x onerror=alert(1)>"
    rows = build(snapshot=snap(), cfg=c)["snapshot"]["params"]
    assert rows[0][1] == "<img src=x onerror=alert(1)>"  # unchanged; app.js sets it with textContent
