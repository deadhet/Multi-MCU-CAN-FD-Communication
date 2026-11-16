# comm/ module and Stage 4 results

Covers `firmware/common/comm/`. Section numbers (§) refer to the Design Document. `comm/` is the only module that knows the CAN frame format. It calls `drivers/` only, and only from the main loop.

## What it does

| Job | Where | Design |
|---|---|---|
| IDs: `(type << 4) \| node`, parameter block `0x021`–`0x024`, heartbeat `0x031`–`0x034` | `MAKE_ID()` | §5.2 |
| Hardware filters for those two ranges; resync IDs `0x011`–`0x014` rejected | `comm_init()` | §5.7 check 0 |
| RAM queue per range: queue 0 = all heartbeats + our own IDs (duplicate node only), queues 1–3 = each peer's parameter blocks. Read heartbeats first, every frame validated | `comm_init()`, `comm_receive()` | §4.3 (changed, see below) |
| Pack the 32-byte parameter block and 16-byte heartbeat, little-endian, field by field | `comm_send_param_block()`, `comm_send_heartbeat()` | §5.3 |
| CRC-16/CCITT-FALSE over the ID (2 bytes LE) + payload before the CRC, as the last packing step | `frame_crc()`, `crc16.c` | §5.5 |
| CRC check value `"123456789"` → `0x29B1` tested at start-up; init fails if wrong | `comm_init()` | §5.5 |
| Sequence per message type, +1 for each frame queued | `send_frame()` | §5.6 |
| Validation checks 1–7 on the local copy, then decode from that copy | `validate()` | §5.7 |
| Per-reason counters and per-sender stream state (NEVER_HEARD, last sequence, accepted) | `comm_errors_t`, `streams[][]` | §5.6, §5.7 |
| Saturating error sum for the heartbeat | `comm_error_summary()` | §5.3 |
| Reset a sender to NEVER_HEARD, for diag/ to call on restart or OFFLINE | `comm_forget_peer()` | §5.6 |
| Restart rule (option B): a heartbeat that passes checks 1–6, has a lower sequence and says BOOT or shows a lower uptime resets that sender and is accepted as its first frame; counted per sender (`r=`) and flagged to diag/ (`comm_frame_t.restart`) | `validate()` | §7.3, decision E3 |
| First heartbeat at boot (0 ms), before the first parameter block (10 ms), so peers learn of a restart before its data arrives | `app_master.c`, `app_slave.c` | §7.3 |
| 4-line debug summary, once a second, never per frame | `comm_debug_report()` | §8.6 |

## Frame layouts as implemented

Parameter block, 32 bytes:

| Bytes | Field | Value now |
|---|---|---|
| 0 | Version | 1 |
| 1 | Source node | `NODE_ID` |
| 2–3 | Status word | Caller's bits 0–9; bits 10–15 forced to 0 |
| 4–7 | Sequence | 1, 2, 3 … |
| 8–27 | Five values, 4 bytes each | From params/ (`params_build_block()`), since Stage 5 |
| 28–29 | CRC-16 | Over ID + bytes 0–27 |
| 30–31 | Padding | 0 |

Heartbeat, 16 bytes:

| Bytes | Field | Value now |
|---|---|---|
| 0 | Version | 1 |
| 1 | Source node | `NODE_ID` |
| 2 | State | `BOOT` = 0, `RESYNCING` = 1, `OPERATIONAL` = 2 (wire codes chosen here) |
| 3 | Synced count | From params/ (`params_synced_count()`), since Stage 5 |
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
| 8 | Per value (source status, NaN/Inf, range) | Runs in params/ since Stage 5 and is counted there as `value_rejected` (`docs/params_design.md`) |
| — | Lost before comm saw it | `rx_overflow` = hardware FIFO overflow events + RAM queue overwrites |

A frame only changes stream state after passing check 7. Rejected frames change nothing: for params/ and diag/ they never arrived, so a node whose heartbeats all fail the CRC counts as silent.

**Rejected frames are logged**, by reason, within §8.6: the first rejection of a reason at once, then at most one line per second per reason with the count since the previous line; the 1 s report flushes the rest. A node sending 100 bad frames a second costs one line a second.

```
MCU1 t=500 rx rejected crc id=50 n=1
```

Reason as on the `err` line, `id` = CAN ID of the latest one (decimal; 50 = 0x032, node 2's heartbeat), `n` = rejections since the previous line for that reason.

## Stand-ins in the apps

Until Stage 5 the apps sent values 0, status 0 and synced count 0; now params/ supplies them and takes every accepted parameter block (`docs/params_design.md`). Since the diag/ stage the heartbeat carries the node's real state (BOOT, RESYNCING, OPERATIONAL) and accepted heartbeats go to diag/ (`docs/diag_design.md`). The error summary in the heartbeat is comm's sum plus params/' `value_rejected`, saturating at 65535.

The report task first runs at 5 ms, then every 1000 ms: its start value is `5 − 1000`, so `(now − last) >= 1000` first holds at 5 ms. Reports therefore fall halfway between two 10 ms sends, so no frame is in flight when counters are printed, and sender and receiver counts can be compared exactly.

## Results (Renode 1.17.0, 100 µs quantum)

Scripts are in `renode/tests/` (moved there 13 Nov 2026). Rerunning one writes its logs to `logs/`.

| Scenario | Script | Result |
|---|---|---|
| 2 nodes | `stage4_two_nodes.resc` | At t=2005, each sent 200 parameter blocks and 21 heartbeats (one at boot); the other accepted 200/200 and 21/21. 6 of 6 pairs match, all error counters 0 |
| 4 nodes | `stage4_four_nodes.resc` | At t=3005, every node sent 300 and 31; every receiver accepted 300/300 and 31/31 from each of the other three. `stage4_crosscheck.awk`: 48 of 48 pairs match, all error counters 0 |
| MCU3 off the bus 1.5–2.5 s | `stage4_reconnect.resc` | MCU3's Tx FIFO filled with 4 frames; 53 further sends counted as `tx_full`. After reconnect the driver's `TXBAR = TXBRP` restart sent them. Peers then held exactly MCU3's 202 and 22 queued frames, `lost` = 0. MCU3 counted `lost` = 330 = 3 peers × (100 + 10) |
| MCU3 CPU halted 1.5–2.0 s | `stage4_halt.resc` | 165 frames arrived during the halt; the 16-deep FIFO kept 16. MCU3: `fifo_lost` = 1 event, `lost` = 149 = 165 − 16. Peers: no gaps |
| MCU3 main loop busy 1.5–2.0 s | `stage4_overload.sh stall` | The interrupt kept running; each RAM queue kept its newest 8. Node 1's first frames after the stall: seq 192–200, aged 80 → 0 ms (with the old 32-slot drop-newest ring: seq 150–159, aged 500 → 410 ms). `ovw` = 7/42/43/43, `lost` = 135 = their sum |
| Same, and node 2 floods 20 frames/ms from 1.6 to 1.9 s | `stage4_overload.sh babble` | MCU3: node 2's queue gave up 6043; heartbeats, n1, n4 exactly as in the stall run (7, 42, 43). MCU1, not stalled: gave up 301 of node 2's frames, n3 250/250, n4 300/300 |
| MCU3 reset at 1.5 s | `stage4_reset.resc` | MCU3's boot heartbeat (seq 1, BOOT) triggered the restart rule at once: every peer `r` = 1, `old` = 0, `dup` = 0, `lost` = 0; its parameter blocks were accepted from seq 1. (Before option B: `old` = 161, `dup` = 2.) The frames due at exactly 1.5 s were cut off by the reset: 299 and 31 accepted |
| Two nodes as node 2 | `stage4_dup_node.resc` | Each copy of node 2: `dupnode` = 221. MCU1 and MCU4: `dup` = 221, `r` = 0 (the copies run in step, so no false restart) |
| Loopback self test | `stage3_loopback.resc` | `LOOPBACK PASS`. Queue phase: 1 frame left in queue 1, then 40 frames into queue 0. Queue 0 kept frames 32–39 in order (32 overwritten, every byte checked); queue 1's frame survived |
| Queue logic on the PC | `firmware/tests/host/run_host_tests.sh` | `FRAME QUEUE PASS`, 8 cases including a frame landing during the copy |

## Mutation tests

`renode/tests/stage4_mutate.sh <name>` copies the tree to `build/mutant/<name>`, breaks only node 2's frames, builds, and runs 4 nodes for 1.1 s. At t=1005, MCU1, MCU3 and MCU4 showed:

| Mutation | Accepted from node 2 (p, h) | Counter at each receiver |
|---|---|---|
| CRC bit flipped | 0, 11 | `crc` = 100 |
| Version 2 | 0, 11 | `ver` = 100 |
| Source byte 3 | 0, 11 | `src` = 100 |
| Length 24 | 0, 11 | `len` = 100 |
| BRS cleared (driver) | 0, 0 | `fmt` = 111 |
| Sequence always 1 | 1, 11 | `dup` = 99 |
| Sequence counting down | 1, 11 | `old` = 99 |
| Sequence +2 each time | 100, 11 (last 200) | `lost` = 99, frames accepted |
| ID `0x025` (node 5) | 0, 11 | MCU1 (extra filter `0x025`–`0x02F`, heartbeat queue): `id` = 100. MCU3, MCU4: dropped by hardware, all counters 0 |
| Every 5th heartbeat's sequence set to 1 after sealing (fake restart, bad CRC) | 100, 9 | `crc` = 2, `lost` = 2, `r` = 0: a corrupted frame cannot trigger the restart rule |

Node 2's own counters stayed 0 in every case, and the other streams were unaffected.

## Changed from the Design Document

§4.3 specifies one 32-slot CAN Rx ring that drops the newest frame when full. Now there are 4 RAM queues of 8 slots: one for heartbeats (and our own IDs), one per peer for parameter blocks. Each keeps its newest frames by lap detection, with no interrupt masking and one writer per index (`docs/fdcan_design.md`, receive path). Reasons: after a main-loop stall the old ring delivered data 410–500 ms old, and one flooding node could fill the shared ring for everyone. The hardware Rx FIFO stays in blocking mode. The Design Document PDF itself is not changed.

## Decided since the Design Document

These were open (the first two in Design Document §15). They are closed, and described above or in the module named:

- **Restart against the sequence rule** (item 1): the restart rule, option B. If the boot heartbeat is lost, the next heartbeat (still BOOT, or with a lower uptime) triggers it; if all are lost, diag/ marks the node OFFLINE and calls `comm_forget_peer()` (≈350 ms).
- **Old queued frames after reconnect** (item 2): the 4 frames held in the Tx FIFO go out first and carry values from before the disconnect. They pass every check, so comm/ accepts them; the settle rule in params/ keeps them from counting as FRESH (`docs/params_design.md`).
- **Check 8** (value range): done in params/.

## Future scope

- **Cancel old Tx frames at the sender** (`TXBCR`) when the bus comes back, so they are never sent. Not used so far because cancel jams Renode's Tx FIFO; to be tried on the board.
- **A queue that never overwrites, for command or event messages on CAN**, if such messages are ever added. The present queues keep the newest frame, which is right for periodic data but would lose a command silently (Design Document §15, item 3).
- **Explicit resync request** on the reserved IDs 0x011–0x014, if parameter periods grow (Design Document §12).
