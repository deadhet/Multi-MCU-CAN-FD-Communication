# comm/ module and Stage 4 results

Covers `firmware/common/comm/`. Section numbers (§) refer to the Design Document. `comm/` is the only module that knows the CAN frame format. It calls `drivers/` only, and only from the main loop.

## What it does

| Job | Where | Design |
|---|---|---|
| IDs: `(type << 4) \| node`, parameter block `0x021`–`0x024`, heartbeat `0x031`–`0x034` | `MAKE_ID()` | §5.2 |
| Hardware filters for those two ranges; resync IDs `0x011`–`0x014` rejected | `comm_init()` | §5.7 check 0 |
| Pack the 32-byte parameter block and 16-byte heartbeat, little-endian, field by field | `comm_send_param_block()`, `comm_send_heartbeat()` | §5.3 |
| CRC-16/CCITT-FALSE over the ID (2 bytes LE) + payload before the CRC, as the last packing step | `frame_crc()`, `crc16.c` | §5.5 |
| CRC check value `"123456789"` → `0x29B1` tested at start-up; init fails if wrong | `comm_init()` | §5.5 |
| Sequence per message type, +1 for each frame queued | `send_frame()` | §5.6 |
| Validation checks 1–7 on the local copy, then decode from that copy | `validate()` | §5.7 |
| Per-reason counters and per-sender stream state (NEVER_HEARD, last sequence, accepted) | `comm_errors_t`, `streams[][]` | §5.6, §5.7 |
| Saturating error sum for the heartbeat | `comm_error_summary()` | §5.3 |
| Reset a sender to NEVER_HEARD, for diag/ to call on restart or OFFLINE | `comm_forget_peer()` | §5.6 |
| 4-line debug summary, once a second, never per frame | `comm_debug_report()` | §8.6 |

## Frame layouts as implemented

Parameter block, 32 bytes:

| Bytes | Field | Value now |
|---|---|---|
| 0 | Version | 1 |
| 1 | Source node | `NODE_ID` |
| 2–3 | Status word | Caller's bits 0–9; bits 10–15 forced to 0 |
| 4–7 | Sequence | 1, 2, 3 … |
| 8–27 | Five values, 4 bytes each | From the caller (0 until params/ exists) |
| 28–29 | CRC-16 | Over ID + bytes 0–27 |
| 30–31 | Padding | 0 |

Heartbeat, 16 bytes:

| Bytes | Field | Value now |
|---|---|---|
| 0 | Version | 1 |
| 1 | Source node | `NODE_ID` |
| 2 | State | `BOOT` = 0, `RESYNCING` = 1, `OPERATIONAL` = 2 (wire codes chosen here) |
| 3 | Synced count | From the caller (0 until diag/ exists) |
| 4–7 | Heartbeat sequence | 1, 2, 3 … |
| 8–11 | Uptime ms | `systick_now_ms()` |
| 12–13 | Error summary | `comm_error_summary()` |
| 14–15 | CRC-16 | Over ID + bytes 0–13 |

## Validation order and counters

| # | Check | Counter on failure |
|---|---|---|
| 0 | Hardware filter (`0x021`–`0x024`, `0x031`–`0x034`) | Dropped by the FDCAN, not counted |
| 1 | Type 2 or 3, sender 1–4, not this node | `bad_id`; own ID from another node → `duplicate_node` |
| 2 | FD frame with BRS | `bad_format` |
| 3 | Length 32 or 16 for the type | `bad_length` |
| 4 | Version = 1 | `bad_version` |
| 5 | Source byte = ID's sender | `bad_source` |
| 6 | CRC-16 with Data ID | `bad_crc` |
| 7 | `d = (int32_t)(seq − last)`: 0 → duplicate, < 0 → old, > 1 → count `d − 1` lost and accept | `duplicate`, `old_seq`, `lost_frames` |
| 8 | Per value (source status, NaN/Inf, range) | `value_rejected`: needs `system_table.c`, not in this stage |
| — | Lost before comm saw it | `rx_overflow` = hardware FIFO overflow events + software ring drops |

A frame only changes stream state after passing check 7. Rejected frames change nothing.

## Stage 4 stand-ins in the apps

Until params/ and diag/ exist, the apps send values 0, status 0, state `BOOT` and synced count 0. The main loop drains `comm_receive()` on every pass and discards accepted frames after comm has counted them.

The report task first runs at 5 ms, then every 1000 ms: its start value is `5 − 1000`, so `(now − last) >= 1000` first holds at 5 ms. Reports therefore fall halfway between two 10 ms sends, so no frame is in flight when counters are printed, and sender and receiver counts can be compared exactly.

## Results (Renode 1.17.0, 100 µs quantum)

Scripts are in `renode/`. Logs were copied to `logs/stage4_<scenario>_mcuN.log`.

| Scenario | Script | Result |
|---|---|---|
| 2 nodes | `stage4_two_nodes.resc` | At t=2005, each sent 200 parameter blocks and 20 heartbeats; the other accepted 200/200 and 20/20. 6 of 6 pairs match, all error counters 0 |
| 4 nodes | `stage4_four_nodes.resc` | At t=3005, every node sent 300 and 30; every receiver accepted 300/300 and 30/30 from each of the other three. `stage4_crosscheck.awk`: 48 of 48 pairs match, all error counters 0 |
| MCU3 off the bus 1.5–2.5 s | `stage4_reconnect.resc` | MCU3's Tx FIFO filled with 4 frames; 53 further sends counted as `tx_full`. After reconnect the driver's `TXBAR = TXBRP` restart sent them. Peers then held exactly MCU3's 202 and 21 queued frames, `lost` = 0. MCU3 counted `lost` = 330 = 3 peers × (100 + 10) |
| MCU3 CPU halted 1.5–2.0 s | `stage4_halt.resc` | 165 frames arrived during the halt; the 16-deep FIFO kept 16. MCU3: `fifo_lost` = 1 event, `lost` = 149 = 165 − 16. Peers: no gaps |
| MCU3 reset at 1.5 s | `stage4_reset.resc` | Peers rejected MCU3's restarted streams: `old` = 161, `dup` = 2, until its new sequence passed the old value (150). See the open question below |
| Two nodes as node 2 | `stage4_dup_node.resc` | Each copy of node 2: `dupnode` = 220. MCU1 and MCU4: `dup` = 220 |
| Loopback self test | `stage3_loopback.resc` | `LOOPBACK PASS` |

## Mutation tests

`renode/stage4_mutate.sh <name>` copies the tree to `build/mutant/<name>`, breaks only node 2's frames, builds, and runs 4 nodes for 1.1 s. At t=1005, MCU1, MCU3 and MCU4 showed:

| Mutation | Accepted from node 2 (p, h) | Counter at each receiver |
|---|---|---|
| CRC bit flipped | 0, 10 | `crc` = 100 |
| Version 2 | 0, 10 | `ver` = 100 |
| Source byte 3 | 0, 10 | `src` = 100 |
| Length 24 | 0, 10 | `len` = 100 |
| BRS cleared (driver) | 0, 0 | `fmt` = 110 |
| Sequence always 1 | 1, 10 | `dup` = 99 |
| Sequence counting down | 1, 10 | `old` = 99 |
| Sequence +2 each time | 100, 10 (last 200) | `lost` = 99, frames accepted |
| ID `0x025` (node 5) | 0, 10 | MCU1 (filter widened to `0x021`–`0x02F`): `id` = 100. MCU3, MCU4: dropped by hardware, all counters 0 |

Node 2's own counters stayed 0 in every case, and the other streams were unaffected.

## Open items

1. **Restart vs the sequence rule (needs a decision).** §7.3 lists "sequence went backwards" as a restart sign, but check 7 rejects those frames before anything else can see them. With diag/ as designed, a quickly reset node is rejected until it is marked OFFLINE (≈350 ms) and `comm_forget_peer()` is called. The backwards-sequence rule never fires.
2. **Stale queued frames after reconnect.** The 4 frames held in the Tx FIFO go out first and carry values from before the disconnect (Stage 0 finding, confirmed here). They pass every check. The freshness rule in params/ will need to account for them.
3. **Check 8** waits for `system_table.c`.
