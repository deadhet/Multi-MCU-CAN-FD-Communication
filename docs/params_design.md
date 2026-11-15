# params/ module and Stage 5 results

Covers `firmware/common/system_table.c` and `firmware/common/params/`. Section numbers (§) refer to the Design Document. params/ holds the live database of all 20 parameters. It never touches CAN: the app hands it the blocks that comm/ accepted, and takes the block to send from it. It calls only `drivers/uart` (debug log), and only from the main loop. Times are passed in, so the whole module also runs on a PC.

## What it does

| Job | Where | Design |
|---|---|---|
| Static definitions of P1–P20: ID, name, owner, type, min, max, sample period, source-stale limit (3 × sample period). Flash, `const`, the same on every node | `system_table.c` | §5.4, §6.1 |
| Live part: `db[20]` of value, sequence, last update time, validity, freshness, source status. Age is never stored | `params.c` | §6.1 |
| Own parameters written through `params_set_local()`, received ones through `params_apply_block()`. Other modules get copies from `params_get()` | `params.c` | §6.1 |
| Check 8 per value: source status, NaN/infinity, min/max. Only the failing parameter is marked INVALID | `params_apply_block()`, `value_ok()` | §5.7 |
| Validity (NEVER_RECEIVED, VALID, INVALID) and freshness (FRESH, STALE), stored, updated every 5 ms | `params_update_freshness()` | §6.2 |
| Transport stale after 30 ms; source stale when the owner reports SOURCE_STALE | `params_update_freshness()` | §6.2 |
| Settle rule: after a sender was silent for more than 30 ms, its parameters stay STALE until one of its blocks arrives 5–30 ms after the previous one, so frames flushed from its Tx FIFO at reconnect never count as FRESH | `params_apply_block()`, `params_update_freshness()` | §6.2, §15 open item 2 |
| Own source status: SOURCE_STALE when the own sample is older than 3 × its sample period, SOURCE_FAULT when it failed check 8; sent in the status word | `params_update_freshness()`, `params_build_block()` | §5.3, §6.3 |
| FRESH/STALE and VALID/INVALID changes logged, grouped, one line per kind | `log_changes()` | §6.2, §8.6 |
| Arrival gap per sender (last, worst) | `params_apply_block()` | §6.2 |
| Synced count (VALID and FRESH, 0–20) for the heartbeat | `params_synced_count()` | §5.3 |
| Owner-offline flag per parameter, copied from diag/ by the app; per-node "all five synced" for diag/ | `params_set_owner_offline()`, `params_node_synced()` | §7 |
| `value_rejected` counter, added to comm's error summary by the app | `params_value_rejected()` | §5.7 |
| Simulated sensors: float → sine, int → ramp, bool → toggle, each at its own sample period | `param_source.c` | §6.3 |

## Parameter table

Every node uses the same 5-slot layout, so each type and sample period runs on all four nodes. Names are "Param 1" … "Param 20". P1–P5 belong to MCU1, P6–P10 to MCU2, P11–P15 to MCU3, P16–P20 to MCU4.

| Slot | Type | Sample period | Source stale after | Range (check 8) | Simulated pattern (node N) |
|---|---|---|---|---|---|
| 1 | float32 | 10 ms | 30 ms | −FLT_MAX … FLT_MAX | 25·N · sin, 2 s cycle |
| 2 | float32 | 50 ms | 150 ms | −FLT_MAX … FLT_MAX | 250 + 50·N · sin, 5 s cycle |
| 3 | int32 | 100 ms | 300 ms | INT32_MIN … INT32_MAX | Ramp −1000 … 1000, step 10·N |
| 4 | uint32 | 500 ms | 1500 ms | 0 … UINT32_MAX | Ramp 0 … 10000, step 100·N |
| 5 | bool | 1000 ms | 3000 ms | 0 … 1 | Toggle |

Ranges are each type's full domain for now. Check 8 therefore only rejects what can never be a value: a float NaN or infinity, a bool other than 0 or 1. Real limits per parameter are a change to `system_table.c` alone.

## Database rules as implemented

Received block from node X (comm/ has already passed checks 1–7), for each of its five values:

| Status from owner | Value | Validity | Value stored | `value_rejected` |
|---|---|---|---|---|
| 0 OK | Passes check 8 | VALID | New | — |
| 0 OK or 1 SOURCE_STALE | Fails check 8 (NaN, ±infinity, outside min/max, bool > 1) | INVALID | Old kept | +1 |
| 1 SOURCE_STALE | Passes check 8 | VALID, and STALE at the next freshness run | New | — |
| 2 SOURCE_FAULT | Any | INVALID | Old kept | — (the owner's own report, not a transport error) |
| 3 reserved | Any | INVALID | Old kept | +1 |

In every row the parameter's sequence becomes the block's sequence and its `last_update_ms` the block's arrival time (the frame did arrive). So an INVALID parameter stays FRESH while blocks keep coming and shows as INVALID + FRESH; it is not synced either way. Blocks that claim this node's own parameters are ignored.

Freshness, every 5 ms:

| Parameter | STALE when |
|---|---|
| Never received (or own, never sampled) | Always |
| Received | `now − last_update_ms > 30` (3 × 10 ms period, two lost frames tolerated), or the owner reports SOURCE_STALE, or the owner is settling (below) |
| Own | `now − last_update_ms >` its source-stale limit (3 × sample period). Its status word then says SOURCE_STALE, unless it is SOURCE_FAULT |

All ages use unsigned subtraction, so they stay correct when the ms counter wraps. Own parameters send SOURCE_STALE until their first sample.

## Settle rule (Design Document open item 2)

**Problem.** When a node loses the bus, the frames already in its Tx FIFO (up to 4) stay pending. At reconnect they go out first, back-to-back, with values from before the outage. Their CRC is good and their sequence follows the last one received (sends that found the FIFO full used no number), so comm/ accepts them, and their arrival time is new. Without a rule, receivers would mark 1-second-old values FRESH until the sender's next new block, about 10 ms later.

**Rule.** Per sender, in `params_apply_block()`, on the gap since its previous block:

| Gap | Effect |
|---|---|
| First block ever | Nothing: a sender's Tx FIFO is empty at boot |
| More than 30 ms (the sender's parameters could have gone transport-STALE) | Sender starts **settling** |
| 5–30 ms (the normal 10 ms beat) | Settling ends |
| Under 5 ms | No change |

While a sender is settling, `params_update_freshness()` keeps its parameters STALE. Its blocks are still applied (values, validity, sequence, timestamp), so the block that ends settling also brings new values. `held nX` in the report counts node X's blocks that arrived while it was settling.

**Why it works.** Flushed frames go out back-to-back, a few frames at the CAN FD data rate, so none of them is on the beat. The first block sampled after the reconnect is queued by the next 10 ms send. Measured in the reconnect run: `held n3=3` at MCU1, exactly the 3 old blocks, then FRESH with new values (Results).

**Cost and limits.**
- Every recovery (halt, reconnect) becomes FRESH about one period (10 ms) later, also when there were no old frames.
- A block delayed to under 5 ms after the previous one only postpones the end of settling. The rule fails towards STALE.
- A link that drops again 5–30 ms into the flush could release one old block as FRESH. Not seen in the scenarios; noted as a limit.

**Not used: overwriting at the sender.** FDCAN cannot write over a pending Tx frame; it must be cancelled first (`TXBCR`). That works on hardware, but in Renode cancel jammed the Tx FIFO (Stage 0, `docs/setup_findings.md`), and cancel on dedicated Tx buffers was not tested. The receiver rule works the same in Renode and on hardware.

## How the app uses it

| Task | Period | Calls |
|---|---|---|
| `param_task` | 10 ms | `param_source_task(now)` then `params_build_block()` then `comm_send_param_block()`. Sampling and sending are one task with one time value |
| `freshness_task` | 5 ms | `params_update_freshness(now)` |
| `heartbeat_task` | 100 ms | `synced_count = params_synced_count()`, `error_summary = comm_error_summary() + params_value_rejected()`, saturating at 65535 |
| `diag_task` | 5 ms | `params_node_synced(node)` for each peer, as diag/'s "data still proper" input; then `params_set_owner_offline(node, diag_peer_offline(node), now)` |
| `report_task` | 1 s, at 5 ms offset | `comm_debug_report()`, `params_debug_report()`, `diag_debug_report()` |
| Main loop | Every pass | Accepted parameter blocks from `comm_receive()` go to `params_apply_block(source, block, sequence, rx_time)` |

**Owner-offline flag**. Each parameter carries `owner_offline`, true while diag/ sees its owner as OFFLINE. The app copies it from diag/; params/ never decides it. Values, validity and freshness are untouched: by the time a node is OFFLINE (≈350 ms) its parameters have been STALE for over 300 ms. A node in NO_HEARTBEAT (heartbeats lost, data still proper) is not flagged. Each change is logged: `MCU1 t=5350 db OWNER_OFFLINE 11 12 13 14 15`, and `OWNER_ONLINE` when its heartbeat returns.

**`params_node_synced(node)`**: true when all five of node's parameters are VALID and FRESH here. diag/ uses it to tell NO_HEARTBEAT from OFFLINE.

Why one task for sampling and sending: as two 10 ms tasks, sensor first, the first Renode run showed MCU3's block at 3000 ms carrying the previous samples. The SysTick moved from 2999 to 3000 ms between the two due checks of one main-loop pass, so the send task ran and the sensor task waited for the next pass.

## Debug report

Two lines after comm's four, once a second, plus change lines from the freshness task:

```
MCU1 t=1525 db STALE 11 12 13 14 15
MCU1 db synced=20 valid=20 invalid=0 stale=0 never=0 offline=0 rej=0 gap n2=10/11 n3=10/11 n4=10/11 held n2=0 n3=0 n4=0
MCU1 val 1=3054676940 2=1130142809 ... 20=1
```

`offline` counts the parameters with the owner-offline flag. `gap nX=a/b` is the last and the worst arrival gap of node X's blocks in ms (1 ms resolution, so 10/11 is normal). `held nX` counts node X's blocks that arrived while it was settling (settle rule). `val` prints the raw 4 bytes of every value as uint32, so floats appear as bit patterns and the logs of different nodes can be compared exactly.

## Results (Renode 1.17.0, 100 µs quantum)

| Scenario | Script | Result |
|---|---|---|
| 4 nodes, 3.1 s | `stage4_four_nodes.resc` | `stage5_params_crosscheck.awk`: 180 of 180 parameter copies equal their owner's value at t = 1005, 2005, 3005 (4 receivers × 15 × 3). Every node `synced=20` from t = 15 ms on; no STALE or INVALID after boot; `held` 0. Worst gap 11 ms. `stage4_crosscheck.awk` still 48 of 48 |
| MCU3 CPU halted 1.5–2.0 s | `stage4_halt.resc` | MCU1: P11–P15 STALE at t = 1525, FRESH at 2015, `held n3=1`: the first block after the halt only started the settling (without the settle rule: FRESH at 2005). Worst gap from node 3 = 510 ms. MCU3 sees nothing stale: its own clock stopped with its CPU |
| MCU3 off the bus 1.5–2.5 s | `stage4_reconnect.resc` | MCU1: P11–P15 STALE at 1525, FRESH at 2515, `held n3=3`: exactly the 3 blocks left in MCU3's Tx FIFO (seq 150–152, sampled 1500–1520 ms). They arrived under 5 ms apart, so they were stored but stayed STALE; the next block, sampled after the reconnect, ended the settling. Without the rule: FRESH at 2505 with those old values. Gap 1010 ms; values match the owner at 3005. MCU3: the other 15 STALE at 1525, FRESH at 2515, `held` 1 per peer (their FIFOs held nothing old: the 10 ms cost of the rule) |
| Node 2 sends bad values (mutation `values`) | `stage4_mutate.sh values` | MCU1, MCU3, MCU4: `synced=15 valid=16 invalid=4 stale=1 rej=300`. P6 (NaN), P7 (SOURCE_FAULT), P9 (status 3), P10 (bool 2) INVALID; P8 (SOURCE_STALE) VALID but STALE. All comm counters 0: the frames themselves are fine. MCU2's own database unaffected |
| MCU3 reset at 1.5 s | `stage4_reset.resc` | Restart rule as before (`r=1`, `old=0`, `dup=0`). Node 3's worst gap 20 ms, so P11–P15 never went STALE and nothing was held: a reset is shorter than the 30 ms limit. `synced=20` at 3005 |
| Node 2's CRC broken (mutation `crc`) | `stage4_mutate.sh crc` | Receivers: `crc=100`, P6–P10 NEVER_RECEIVED, `synced=15 never=5`. The golden rule holds: a rejected frame never reaches the database |
| Sequence fake restart (mutation `fakeboot`) | `stage4_mutate.sh fakeboot` | `crc=2`, `r=0`, `synced=20`: unchanged from Stage 4 |
| Two MCUs as node 2 | `stage4_dup_node.resc` | As in Stage 4 (`dupnode=221`, `dup=221`); P11–P15 NEVER_RECEIVED (no node 3), `synced=15` |
| MCU1 and MCU2 only | `stage4_two_nodes.resc` | 6 of 6 pairs; `synced=10`, the 10 parameters of the missing nodes NEVER_RECEIVED |
| Loopback self test | `stage3_loopback.resc` | `LOOPBACK PASS` |
| Database rules on the PC | `firmware/tests/host/run_host_tests.sh` | `PARAMS PASS`: table, init, 30 ms boundary, settle rule (reconnect replay, 30/31 ms and 4/5 ms boundaries), own and unknown senders, check 8 cases, own source stale and fault, arrival gaps across the ms wrap, simulated sensors. With the 30 ms rule changed to `>=` and the reject counter removed, 7 checks fail. With the settle rule ignored by the freshness task, 9 fail; with its 5 ms bound written `>` or its 30 ms bound written `>=`, 2 fail each |

## Changed or decided beyond the Design Document

| Item | Design Document | Now |
|---|---|---|
| Ranges | min/max per parameter | Full type domain for now |
| INVALID value | "The old value is kept but flagged" | Also: timestamp and sequence move on, so INVALID + FRESH while blocks arrive |
| `value_rejected` | Listed with comm's counters | Counted in params/; the heartbeat summary is comm's sum + params/' counter |
| SOURCE_FAULT from the owner | INVALID | INVALID, not counted in `value_rejected`; reserved code 3 is INVALID and counted |
| Own parameter before its first sample | Not specified | Sent as SOURCE_STALE |
| Own value that fails check 8 | Not specified | Old value kept, INVALID, sent as SOURCE_FAULT, not counted |
| `val` debug line | Not specified | Raw 4 bytes as uint32, for exact comparison between nodes |
| Sensor sampling | Separate per-parameter sample periods | Kept, but called from the 10 ms send task (see above) |
| Old Tx frames after reconnect | Open item 2 | Settle rule at the receiver |

## Open items

1. **Restart and OFFLINE: decided**. An OFFLINE node's values are kept (STALE) and marked with the owner-offline flag; see "How the app uses it".
