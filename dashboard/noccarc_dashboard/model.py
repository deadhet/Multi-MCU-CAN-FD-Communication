"""What the dashboard shows: every text, and which states are abnormal, decided here and tested
(tests/test_model.py). web/app.js only draws the result.

build(view, source) turns Link.view() into the page model, a JSON-ready dict:
    header:   source, link, health_head, uptime, pc_time; stale (old data from a lost link)
    snapshot: None until MCU1's first snapshot; then note, health (section 1), nodes (section 2
              rows), params (section 3 rows)
    config:   CAN FD configuration rows (section 1), None until the first CONFIG
    uart:     monitoring link rows (section 1)
    events:   the event log (section 2), newest first

Every value shown comes from MCU1's frames. The PC adds only what it measures itself: the link
state, frame counts, MCU1's total uptime across the 32-bit wrap, and the PC's clock. Before the
first snapshot nothing is shown; after the link is lost the last data stays, marked as old.

A cell is a string, or {"t": text, "l": level}: level "" (normal, plain text), "caution",
"fault", "info" (in transition) or "dim" (no data, not applicable). "m": True asks for a small
mark only, for columns derived from a state already marked in full and for the event log
(one strong mark per fault). Rule of the page (style.css): normal states are never coloured.
"""

from __future__ import annotations

import math
from datetime import datetime

from . import monitor_pb2 as pb
from .uptime import format_uptime

EVENTS_SHOWN = 200
NODES = 4    # the health formula's denominators (monitor_health_percent() on MCU1)
PARAMS = 20

Cell = str | dict


def cell(text: str, level: str = "", mark: bool = False) -> Cell:
    if not level:
        return text
    c = {"t": text, "l": level}
    if mark:
        c["m"] = True
    return c


NA = cell("n/a", "dim")
NONE = cell("–", "dim")

# How abnormal each link state is (diag/ on MCU1, Design Document §7.2).
LINK_LEVEL = {
    pb.UNKNOWN: "info",        # not heard yet since MCU1's boot
    pb.ONLINE: "",
    pb.LATE: "caution",        # a heartbeat 150 ms overdue
    pb.OFFLINE: "fault",       # 3 heartbeats missed and its data not arriving
    pb.RECOVERING: "info",     # back, waiting for a heartbeat that says OPERATIONAL
    pb.NO_HEARTBEAT: "caution",  # heartbeats stopped, its data still arrives VALID and FRESH
    pb.DATA_INVALID: "fault",  # heartbeats arrive, no valid parameter block for 350 ms
    pb.SELF: "",
}

STATE_LEVEL = {pb.BOOT: "info", pb.RESYNCING: "info", pb.OPERATIONAL: ""}

PARAM_CHANGE = {  # ParamChange -> (words, level)
    pb.BECAME_VALID: ("became VALID", ""),
    pb.BECAME_INVALID: ("became INVALID", "fault"),
    pb.BECAME_FRESH: ("became FRESH", ""),
    pb.BECAME_STALE: ("became STALE", "caution"),
    pb.OWNER_WENT_OFFLINE: ("owner went offline", "fault"),
    pb.OWNER_BACK: ("owner back online", ""),
}


# -- formatting ------------------------------------------------------------------------------------
def fmt_float(v: float) -> str:
    if not math.isfinite(v):
        return str(v)
    text = f"{v:.6g}" if abs(v) >= 1e6 else f"{v:.3f}"
    return "0.000" if text == "-0.000" else text


def fmt_bps(bps: int) -> str:
    if bps and bps % 1_000_000 == 0:
        return f"{bps // 1_000_000} Mbit/s"
    if bps and bps % 1000 == 0:
        return f"{bps // 1000} kbit/s"
    return f"{bps} bit/s"


def fmt_age(ms: int) -> str:
    return f"{ms} ms" if ms < 10_000 else f"{ms / 1000:.1f} s"


def health_label(pct: int) -> tuple[str, str]:
    """Het, 13 Nov 2026: 100 % HEALTHY, 1-99 % DEGRADED, 0 % DOWN."""
    if pct >= 100:
        return "HEALTHY", ""
    if pct <= 0:
        return "DOWN", "fault"
    return "DEGRADED", "caution"


def members(mask: int) -> list[int]:
    return [n + 1 for n in range(32) if (mask >> n) & 1]


# -- section 1 -----------------------------------------------------------------------------------
def health(s: pb.Snapshot) -> dict:
    label, level = health_label(s.health_percent)
    o, p = s.online_count, s.synced_count
    rule = (f"Health = the weaker of controllers online ({o}/{NODES} = {o * 100 // NODES} %) and "
            f"parameters synchronized ({p}/{PARAMS} = {p * 100 // PARAMS} %), each rounded down. "
            f"Online = MCU1 and every peer whose link is ONLINE.")
    nodes = []
    for n in s.nodes:
        name = "master" if n.link == pb.SELF else pb.LinkState.Name(n.link)
        lv = "dim" if n.link == pb.UNKNOWN else LINK_LEVEL.get(n.link, "fault")
        nodes.append({"t": str(n.node), "l": lv, "title": f"MCU{n.node} {name}"})
    params = [param_seg(x) for x in s.params]
    return {
        "pct": s.health_percent,
        "label": {"t": label, "l": level},
        "rule": rule,
        "online": f"{o}/{NODES}",
        "synced": f"{p}/{PARAMS}",
        "nodes": nodes,
        "params": [params[i:i + 5] for i in range(0, len(params), 5)],
    }


def param_seg(p: pb.Parameter) -> dict:
    if p.validity == pb.NEVER_RECEIVED:
        lv, what = "dim", "never received"
    elif p.validity == pb.INVALID:
        lv, what = "fault", "INVALID"
    elif p.owner_offline:
        lv, what = "fault", "owner offline"
    elif p.freshness == pb.STALE:
        lv, what = "caution", "STALE"
    else:
        lv, what = "", "synchronized"
    return {"t": "", "l": lv, "title": f"P{p.id} {what}"}


def config_rows(c: pb.Config | None) -> list | None:
    if c is None:
        return None
    return [
        ["Arbitration bit rate", fmt_bps(c.arbitration_bps)],
        ["Data bit rate", fmt_bps(c.data_bps)],
        ["Bit rate switch (BRS)", "on" if c.brs else "off"],
        ["Parameter / heartbeat period", f"{c.param_period_ms} ms / {c.heartbeat_period_ms} ms"],
        ["Firmware, protocol", f"{c.firmware}, v{c.protocol_version}"],
    ]


def uart_rows(view: dict, source: str) -> list:
    errors = sum(view["errors"].values())
    detail = ", ".join(f"{k} {v}" for k, v in sorted(view["errors"].items()))
    return [
        ["Source", source],
        ["Frames received", str(view["frames"])],
        ["Lost on the way", cell(str(view["lost"]), "caution" if view["lost"] else "", mark=True)],
        ["Replaced by MCU1 (latest wins)", str(view["replaced"])],
        ["Frame errors", cell(f"{errors} ({detail})", "caution", mark=True) if errors else "0"],
        ["MCU1 restarts seen", cell(str(view["restarts"]), "info" if view["restarts"] else "", mark=True)],
    ]


# -- section 2 -----------------------------------------------------------------------------------
def recovery(state: int) -> Cell:
    """Recovery and re-synchronization status from the state the node reports."""
    if state == pb.OPERATIONAL:
        return "Normal (OPERATIONAL)"
    if state == pb.RESYNCING:
        return cell("Re-synchronizing (RESYNCING)", "info", mark=True)
    return cell("Booting (BOOT)", "info", mark=True)


def node_row(n: pb.NodeHealth, mcu1_restarts: int) -> list:
    if n.link == pb.SELF:
        return [f"MCU {n.node} (master)", "This node", NA, NA, NA, NA, recovery(n.reported_state),
                str(mcu1_restarts)]
    link = n.link
    status = cell(pb.LinkState.Name(link), LINK_LEVEL.get(link, "fault"))
    if link == pb.UNKNOWN:
        heartbeat = cell("Not heard yet", "info", mark=True)
    elif link == pb.LATE:
        heartbeat = cell("Late", "caution", mark=True)
    elif link in (pb.OFFLINE, pb.NO_HEARTBEAT):
        heartbeat = cell("Not received", "fault", mark=True)
    else:
        heartbeat = "Receiving"
    age = cell("never", "dim") if link == pb.UNKNOWN else f"{n.heartbeat_age_ms / 1000:.3f} s"
    if n.missed == 0:
        missed: Cell = "0"
    else:
        missed = cell(str(n.missed), "fault" if n.missed >= 3 else "caution", mark=True)
    timeout = {
        pb.ONLINE: "None",
        pb.RECOVERING: "None",
        pb.LATE: cell("Heartbeat late", "caution", mark=True),
        pb.NO_HEARTBEAT: cell("Heartbeat lost, data arriving", "caution", mark=True),
        pb.OFFLINE: cell("Heartbeat and data", "fault", mark=True),
        pb.DATA_INVALID: cell("No valid data block", "fault", mark=True),
    }.get(link, cell("Not heard yet", "dim"))
    if link == pb.RECOVERING:
        rec = cell("Recovering: waiting for OPERATIONAL", "info", mark=True)
    elif link == pb.OFFLINE:
        rec = cell("Waiting for heartbeat", "dim")
    elif link == pb.UNKNOWN:
        rec = cell("Not heard yet", "dim")
    else:
        rec = recovery(n.reported_state)
    return [f"MCU {n.node}", status, heartbeat, age, missed, timeout, rec, str(n.restarts)]


def event_row(pc_time: str, e: pb.Event) -> list:
    kind = e.WhichOneof("body")
    if kind == "node":
        who = " ".join(f"MCU{n}" for n in members(e.node.nodes))
        if e.node.WhichOneof("change") == "restarted":
            text, level = f"{who} RESTARTED", "caution"
        else:
            text, level = f"{who} {pb.LinkState.Name(e.node.link)}", LINK_LEVEL.get(e.node.link, "fault")
    elif kind == "own_state":
        text, level = f"MCU1 {pb.NodeState.Name(e.own_state)}", STATE_LEVEL.get(e.own_state, "info")
    elif kind == "param":
        words, level = PARAM_CHANGE.get(e.param.change, ("changed", "info"))
        text = " ".join(f"P{p}" for p in members(e.param.params)) + " " + words
    else:
        text, level = "(empty event)", "dim"
    return [f"{e.time_ms / 1000:.3f}", pc_time[11:23], cell(text, level, mark=True)]


# -- section 3 -----------------------------------------------------------------------------------
TYPE_NAME = {"f32": "f32", "i32": "i32", "u32": "u32", "flag": "bool"}


def param_row(p: pb.Parameter, names: dict[int, str]) -> list:
    kind = p.WhichOneof("value")
    never = p.validity == pb.NEVER_RECEIVED
    if never or kind is None:
        value: Cell = NONE
    elif kind == "f32":
        value = fmt_float(p.f32)
    elif kind == "flag":
        value = "true" if p.flag else "false"
    else:
        value = str(getattr(p, kind))
    name = names.get(p.id)
    source = cell(f"MCU {p.source} offline", "fault", mark=True) if p.owner_offline else f"MCU {p.source}"
    fresh = "FRESH" if p.freshness == pb.FRESH else cell("STALE", "caution")
    valid = {pb.VALID: "VALID", pb.INVALID: cell("INVALID", "fault")}.get(p.validity, cell("NEVER RECEIVED", "dim"))
    return [f"P{p.id}", name if name else NONE, value, TYPE_NAME.get(kind, "–"), source, fresh, valid,
            NONE if never else fmt_age(p.age_ms)]


# -- the whole page ------------------------------------------------------------------------------
def build(view: dict, source: str, now: datetime | None = None) -> dict:
    now = now or datetime.now()
    state = view["state"]
    s: pb.Snapshot | None = view["snapshot"]
    c: pb.Config | None = view["config"]

    if state == "CONNECTED":
        link: Cell = "CONNECTED"
    elif state == "LOST":
        age = view["since_valid_s"]
        link = cell("LOST: no valid frame yet" if age is None else f"LOST: no valid frame for {age:.1f} s", "fault")
    else:
        link = cell("DISCONNECTED: retrying every 1 s", "fault")

    if s is None:
        health_head: Cell = cell("NO DATA", "dim")
    elif state != "CONNECTED":
        health_head = cell(f"NO DATA (last {s.health_percent} %)", "dim")
    else:
        label, level = health_label(s.health_percent)
        health_head = cell(f"{s.health_percent} % {label}", level)

    snapshot = None
    if s is not None:
        names = {p.id: p.name for p in c.params} if c is not None else {}
        snapshot = {
            "note": f"snapshot at MCU1 t = {s.uptime_ms / 1000:.3f} s",
            "health": health(s),
            "nodes": [node_row(n, view["restarts"]) for n in s.nodes],
            "params": [param_row(p, names) for p in s.params],
        }

    return {
        "pc_time": now.strftime("%Y-%m-%d %H:%M:%S"),
        "source": f"MCU1 over UART, {source}",
        "link": link,
        "stale": s is not None and state != "CONNECTED",
        "health_head": health_head,
        "uptime": format_uptime(view["uptime_ms"]) if s is not None else None,
        "config": config_rows(c),
        "uart": uart_rows(view, source),
        "events": event_log(view["events"]),
        "snapshot": snapshot,
    }


def event_log(entries: list) -> list:
    """Newest first, at most EVENTS_SHOWN events. MCU1's times start again at 0 when it restarts,
    so a separator row marks where one MCU1 boot ends and the next begins."""
    rows: list = []
    newer_session = None
    for pc, e, session in reversed(entries[-EVENTS_SHOWN:]):
        if newer_session is not None and session != newer_session:
            rows.append(["", "", cell("MCU1 restarted: its times start again at 0 (newer above)", "info", mark=True)])
        rows.append(event_row(pc, e))
        newer_session = session
    return rows
