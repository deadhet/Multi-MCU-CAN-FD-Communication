"""EVENT frames (§8.3): in words, and replayed as state.

MCU1 looks for changes right after anything that can make one (change counts in diag/ and params/;
Het, 13 Nov 2026: events option b), compares its state with the previous look and sends one event
per kind of change, naming every node or parameter that made it as a bit mask (bit n-1 = MCUn or Pn).
Every change is reported, however short.

Replaying events over a snapshot must give the next snapshot's states exactly: links, MCU1's own
state, and per parameter valid / fresh / owner offline. Before every snapshot MCU1 looks once more,
on the snapshot's own view, and those events go out before the snapshot. So the events received
before a snapshot explain it completely, with no time allowance.
"""

from __future__ import annotations

from . import monitor_pb2 as pb

State = dict[str, int | bool]

_PARAM_EFFECT = {  # change -> (state key, new value)
    pb.BECAME_VALID: ("valid", True),
    pb.BECAME_INVALID: ("valid", False),
    pb.BECAME_FRESH: ("fresh", True),
    pb.BECAME_STALE: ("fresh", False),
    pb.OWNER_WENT_OFFLINE: ("owner_offline", True),
    pb.OWNER_BACK: ("owner_offline", False),
}


def members(mask: int) -> list[int]:
    """Bit n-1 set -> n."""
    return [n + 1 for n in range(32) if (mask >> n) & 1]


def describe(e: pb.Event) -> str:
    kind = e.WhichOneof("body")
    if kind == "node":
        who = " ".join(f"n{n}" for n in members(e.node.nodes))
        what = "RESTARTED" if e.node.WhichOneof("change") == "restarted" else pb.LinkState.Name(e.node.link)
        return f"t={e.time_ms} {who} {what}"
    if kind == "own_state":
        return f"t={e.time_ms} MCU1 {pb.NodeState.Name(e.own_state)}"
    if kind == "param":
        who = " ".join(f"P{p}" for p in members(e.param.params))
        return f"t={e.time_ms} {who} {pb.ParamChange.Name(e.param.change)}"
    return f"t={e.time_ms} (empty)"


def state_of(s: pb.Snapshot) -> State:
    st: State = {}
    for n in s.nodes:
        if n.link == pb.SELF:
            st["own"] = n.reported_state
        else:
            st[f"n{n.node}"] = n.link
    for p in s.params:
        st[f"P{p.id} valid"] = p.validity == pb.VALID
        st[f"P{p.id} fresh"] = p.freshness == pb.FRESH
        st[f"P{p.id} owner_offline"] = p.owner_offline
    return st


def apply(st: State, e: pb.Event) -> None:
    """Every event sets absolute values, so applying one twice changes nothing."""
    kind = e.WhichOneof("body")
    if kind == "own_state":
        st["own"] = e.own_state
    elif kind == "node" and e.node.WhichOneof("change") == "link":
        for n in members(e.node.nodes):
            st[f"n{n}"] = e.node.link
    elif kind == "param" and e.param.change in _PARAM_EFFECT:
        key, value = _PARAM_EFFECT[e.param.change]
        for p in members(e.param.params):
            st[f"P{p} {key}"] = value


def replay_check(frames: list) -> tuple[int, list[str]]:
    """Checks that the events between two snapshots turn the first into exactly the second.
    Frames in arrival order, with .type, .sequence and .message. Snapshots after a gap explained by
    frames_skipped (a frame MCU1 replaced or an event it could not queue), or after MCU1 restarted,
    start a new comparison. Returns (snapshots checked, differences)."""
    from .framing import EVENT, SNAPSHOT

    model: State | None = None
    last_seq = None
    last_skipped = None
    checked, problems = 0, []
    for f in frames:
        if last_seq is not None and (f.sequence - last_seq) % (1 << 32) >= (1 << 31):
            model = None  # MCU1 restarted: its state starts again
        last_seq = f.sequence
        if f.type == EVENT and model is not None:
            apply(model, f.message)
        elif f.type == SNAPSHOT:
            s = f.message
            now = state_of(s)
            if model is not None and s.frames_skipped == last_skipped:
                checked += 1
                for key, value in now.items():
                    if value != model.get(key):
                        problems.append(f"snapshot t={s.uptime_ms}: {key} is {value}, events say {model.get(key)}")
            model = now  # Absolute values: safe to restart from every snapshot
            last_skipped = s.frames_skipped
    return checked, problems
