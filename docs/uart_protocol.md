# UART monitoring link: protocol

MCU1 sends its validated P1–P20 database and the network health to the Python dashboard over one UART. This document specifies the link as implemented: protocol version 1, payload schema `proto/monitor.proto`.

| Assignment §10 asks for | Section |
|---|---|
| Baud rate and configuration | [1](#1-physical-link) |
| Data-frame structure | [2](#2-frame), [3](#3-worked-example) |
| Message types, update frequency | [4](#4-message-types-and-rates) |
| Parameter data representation; network and MCU health format | [5](#5-payload) |
| Frame validation and error handling | [7](#7-receiver-validation-and-accounting) |
| UART communication-loss handling | [8](#8-link-loss) |

```
proto/monitor.proto      payload: one schema, C (nanopb) and Python generated from it
frame                    [version|type][sequence][payload][CRC-16]
COBS + 0x00              frame boundaries in the byte stream
USART3, 230,400 8N1      MCU1 → PC only
```

| Code | Role |
|---|---|
| `firmware/common/monitor/monitor.c`, `cobs.c`, `util/crc16.c` | Encode, frame, CRC, COBS (MCU1 only) |
| `firmware/common/drivers/uart_link.c`, `tx_slots.c` | When bytes leave: latest wins, event queue, USART3 interrupt |
| `firmware/nodes/master/app_master.c` | When each message is made |
| `dashboard/noccarc_dashboard/framing.py`, `link.py`, `uptime.py` | Decode, validate, link state, sequence accounting, uptime |
| `scripts/gen_proto.sh` | Regenerates `monitor.pb.c/.h` and `monitor_pb2.py` (outputs committed) |

The nodes' text debug logs are a different UART (USART1 on every node) and are not part of this protocol.

## 1. Physical link

| Item | Value |
|---|---|
| Peripheral | USART3, TX on PD8, transmit only |
| Baud | 230,400 (BRR = 521 at 120 MHz: −0.03 %, tolerance ±2 %) |
| Format | 8 data bits, no parity, 1 stop bit |
| Flow control | None |
| Capacity | 10 bits per byte: 23,040 B/s, 43.4 µs per byte |
| In Renode | USART3 → TCP port 3456 (`renode/system.resc`); dashboard: `--port 3456` |
| On the board | USB-UART adapter; dashboard: `--serial /dev/ttyUSB0` (pyserial, 8N1) |

## 2. Frame

Decoded frame:

| Offset | Size | Field | Content |
|---|---|---|---|
| 0 | 1 | Header | bits 7–4: protocol version (1); bits 3–0: type (1 SNAPSHOT, 2 EVENT, 3 CONFIG) |
| 1 | 4 | Sequence | uint32, little-endian. 1 for the first frame after a reset; +1 per frame (section 6) |
| 5 | n | Payload | The Protobuf message of that type |
| 5 + n | 2 | CRC | CRC-16/CCITT-FALSE, little-endian, over bytes 0 … 4 + n |

**CRC-16/CCITT-FALSE:** polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR; `"123456789"` gives 0x29B1. It is the same function as the CAN end-to-end CRC (`util/crc16.c`).

**On the wire:** `COBS(frame)` followed by one `0x00`. COBS removes every `0x00` from the frame: each group is a code byte (group length + 1) and up to 254 non-zero bytes, and a code below 0xFF also stands for a `0x00` that followed the group. So `0x00` only ever means "end of frame", and after any error the receiver is back in step at the next `0x00`. Overhead: at most `n/254 + 1` bytes.

| Message | Payload, maximum | On the wire, maximum | Measured payload |
|---|---|---|---|
| SNAPSHOT | 1,046 B | 1,059 B | 309–442 B in S1, ≈437 B on average; 759 B with every field at its largest |
| CONFIG | 807 B | 819 B | 390 B |
| EVENT | 16 B | 25 B | 5–11 B (14–20 B on the wire) |

The maximums come from the nanopb generator (`monitor_Snapshot_size` etc.). Every buffer on MCU1 is sized from them, so a schema change resizes the buffers.

## 3. Worked example

Both examples are real output of the C encoder: `firmware/tests/host/test_monitor.c` writes its frames to `build/host/monitor_frames.bin`, and the dashboard's `framing.py` decodes them (`dashboard/tests/test_framing.py`). The values come from the test's made-up network view.

**EVENT, sequence 5: "MCU3 went OFFLINE" at 1005 ms.** 18 bytes on the wire:

```
03 12 05 01 01 0C 08 ED 07 1A 04 08 04 10 03 E4 B0 00
```

COBS decoded (16 bytes): `12 05 00 00 00 08 ED 07 1A 04 08 04 10 03 E4 B0`

| Bytes | Field | Value |
|---|---|---|
| `12` | Header | version 1, type 2 = EVENT |
| `05 00 00 00` | Sequence | 5 |
| `08` | Tag: `Event.time_ms` (field 1, varint) | |
| `ED 07` | Varint | 0x6D + 0x07 × 128 = 1005 ms |
| `1A 04` | Tag: `Event.node` (field 3, length-delimited), 4 bytes follow | |
| `08 04` | `NodeEvent.nodes` (field 1) | mask 0b0100: MCU3 |
| `10 03` | `NodeEvent.link` (field 2) | 3 = OFFLINE |
| `E4 B0` | CRC | 0xB0E4, over the 14 bytes before it |

How COBS made the wire bytes: `03` = 2 data bytes (`12 05`), then a 0; `01` = no data, then a 0; `01` = no data, then a 0; `0C` = 11 data bytes (`08 … B0`), end of frame; `00` = terminator.

A tag byte is field number × 8 + wire type (0 varint, 2 length-delimited, 5 fixed 32-bit).

**One parameter inside a SNAPSHOT:** P1 = 1.5 (float), VALID, FRESH, age 3 ms, sequence 7, source OK, owner online. 19 bytes:

```
1A 11 08 01 10 01 1D 00 00 C0 3F 38 01 40 01 48 03 50 07
```

| Bytes | Field | Value |
|---|---|---|
| `1A 11` | `Snapshot.params` (field 3), 17 bytes follow | |
| `08 01` | `id` | 1 |
| `10 01` | `source` | MCU1 |
| `1D 00 00 C0 3F` | `f32` (field 3, fixed 32-bit) | 0x3FC00000 = 1.5 |
| `38 01` | `validity` | VALID |
| `40 01` | `freshness` | FRESH |
| `48 03` | `age_ms` | 3 |
| `50 07` | `sequence` | 7 |
| (none) | `source_status`, `owner_offline` | 0 and false: proto3 does not send zero values |

## 4. Message types and rates

| Type | Message | Sent | Purpose |
|---|---|---|---|
| 1 | SNAPSHOT | Every 100 ms, at 5, 105, 205 … ms | The whole state. Self-contained: one good snapshot restores the dashboard |
| 2 | EVENT | Right after a change (below) | The exact time of every state change, including changes shorter than 100 ms |
| 3 | CONFIG | At 0 ms, then every 1 s | Bit rates, BRS, versions, periods, parameter names. Repeated, so a PC that connects late still gets it |

- **SNAPSHOT timing:** 5 ms after a 10 ms CAN send, in the same pass as MCU1's 1 s debug report. The report and that snapshot therefore show the same values, which the end-to-end check compares exactly.
- **EVENT timing:** MCU1 looks for changes after each task pass and after each received CAN frame, when diag/'s or params/'s change count moved. It also looks right before every snapshot, on the snapshot's own data. So the events sent before a snapshot explain it exactly. Each kind of change becomes one event naming every node or parameter that made it.

**Link load** (230,400 baud, 23,040 B/s):

| Traffic | Bytes per second | Load |
|---|---|---|
| SNAPSHOT, ≈447 B on the wire × 10 | ≈4,470 | ≈19 % |
| CONFIG, ≈400 B × 1 | ≈400 | ≈2 % |
| EVENT | ≈20 B per change, only on changes | <1 % |
| **Total** | ≈4,900 | **≈21 %** (ceiling 30 %, as for CAN) |

With every snapshot field at its largest (770 B on the wire), snapshots alone would be 33 %. Then the busy-link rule (section 6) skips a snapshot instead of blocking.

## 5. Payload

`proto/monitor.proto` is the specification, field by field. The rules of the encoding:

- **Zero means "unknown / not good".** proto3 does not send a field equal to 0, and a missing field reads as 0. So every enum has its unsafe value at 0 (`NEVER_RECEIVED`, `STALE`, `BOOT`, `UNKNOWN`, `CHANGE_UNKNOWN`), and a missing field never reads as healthy. One exception, `SOURCE_OK = 0`, copies the CAN block's 2-bit source status. It is safe because a parameter whose source is not OK is INVALID (check 8), and `validity` follows the rule.
- **Same numbering as the firmware:** the schema's enums match the firmware's, checked by `_Static_assert`s in `monitor.c`. Values are copied without a lookup table.
- **Integers** are varints (7 bits per byte: 1–5 bytes). Signed values use `sint32` (zigzag), so small negative numbers stay small. **Floats** are always 4 bytes, IEEE-754, little-endian.
- **Fixed sizes on MCU1:** `proto/monitor.options` bounds every list and string (4 nodes, 20 parameters, 16-character names). nanopb then generates fixed-size C structs (no `malloc`, no callbacks) and the maximum sizes in section 2.

**Snapshot: dashboard Section 1**

| Field | Meaning |
|---|---|
| `uptime_ms` | MCU1's clock, ms (wraps after 49.7 days; section 7) |
| `online_count` | Controllers online out of 4: MCU1 plus every peer ONLINE or LATE |
| `synced_count` | Parameters VALID and FRESH at MCU1, out of 20 |
| `health_percent` | The weaker of `online_count`/4 and `synced_count`/20, each in whole percent, rounded down |
| `frames_skipped` | Frames MCU1 made but never sent, since boot (section 6) |
| `nodes[4]`, `params[20]` | Sections 2 and 3, below |

The health figure is never better than its weakest part. For example, 4/4 online with 0/20 synchronised gives 0 %, where an average would show 50 %. A late heartbeat while the node's data keeps arriving does not lower it. A node that stops sending altogether lowers it within 30 ms, when its five parameters go STALE (synced 15/20: 75 %), before it is even LATE.

**NodeHealth (one per MCU, MCU1 first): dashboard Section 2**

| Field | Meaning |
|---|---|
| `node` | 1–4 |
| `link` | MCU1's view of that node: `UNKNOWN` (never heard), `ONLINE`, `LATE` (150 ms without a heartbeat), `OFFLINE` (350 ms), `RECOVERING` (back, recovery not yet confirmed), `NO_HEARTBEAT` (data arriving, heartbeats not), `DATA_INVALID` (heartbeats arriving, no valid data block for 350 ms), `SELF` (MCU1's own row) |
| `reported_state` | The state the node reported in its last heartbeat: `BOOT`, `RESYNCING`, `OPERATIONAL` |
| `heartbeat_age_ms`, `missed` | Since its last heartbeat (since MCU1's boot if never); heartbeats missed since the last one |
| `restarts`, `recoveries` | Counted by MCU1 |
| `synced_count`, `error_summary` | As the node reported them in its heartbeat |
| `data_age_ms` | Since its last accepted parameter block |

The link states and their timing are specified in `docs/diag_design.md`.

**Parameter (P1–P20): dashboard Section 3**

| Field | Meaning |
|---|---|
| `id`, `source` | 1–20; owner MCU 1–4 |
| `f32` / `i32` / `u32` / `flag` | The value, typed (`oneof`): the PC needs no copy of the parameter table |
| `validity` | `NEVER_RECEIVED`, `VALID`, `INVALID` |
| `freshness` | `STALE`, `FRESH` |
| `age_ms`, `sequence` | Since its last update; the block sequence that updated it (MCU1's own: samples taken) |
| `source_status`, `owner_offline` | The owner's sensor status; its owner is OFFLINE |

**Event**

| Field | Meaning |
|---|---|
| `time_ms` | MCU1's clock when it saw the change |
| `param` | `change` (`BECAME_VALID`, `BECAME_INVALID`, `BECAME_FRESH`, `BECAME_STALE`, `OWNER_WENT_OFFLINE`, `OWNER_BACK`) and `params`, a bit mask (bit n−1 = Pn) |
| `node` | `nodes`, a bit mask (bit n−1 = MCUn), and either the new `link` state or `restarted` |
| `own_state` | MCU1's own state changed |

Within one look, events are sent in this order: restarts, link changes (one event per new state), MCU1's own state, then owner offline, owner back, VALID, INVALID, FRESH, STALE.

**Config**

| Field | Meaning |
|---|---|
| `protocol_version`, `firmware` | 1; firmware version text |
| `arbitration_bps`, `data_bps`, `brs` | 500,000; 2,000,000; true. Computed from the constants that program the FDCAN bit-timing registers, because Renode does not keep writes to those registers (`docs/setup_findings.md`) |
| `node_count`, `param_count` | 4, 20 |
| `param_period_ms`, `heartbeat_period_ms`, `snapshot_period_ms` | 10, 100, 100 |
| `params[20]` | Name, owner and sample period of each parameter |

## 6. Transmit side (MCU1)

- **MCU1 never waits for the link.** The main loop writes a frame into a buffer, hands it over and goes on. The USART3 transmit interrupt moves the bytes. On the board a DMA stream can do it instead; Renode's USART has no transmit DMA request.
- **Latest wins for SNAPSHOT and CONFIG.** Each has two buffers: one may be on its way out, the other holds the next frame. A newer frame replaces one that is still waiting, so after a stall the newest data goes out, never a backlog of old snapshots.
- **EVENTs** wait in a 16-entry queue and are never replaced. If the queue is full, the event is refused. The next snapshot still carries the state.
- **Every frame gets a sequence number,** including one that is replaced or refused. Those frames are counted in the snapshot's `frames_skipped`, so every gap the PC sees is either explained by MCU1 or a real loss.
- Frames leave in the order they were handed over. All buffers are in `.noinit` RAM: not cleared at start-up, and reachable by DMA.

## 7. Receiver: validation and accounting

**Checks, in order.** A frame that fails one is dropped, counted under that reason, and never shown.

| # | Check | Reason counted |
|---|---|---|
| 1 | Split the stream at `0x00` | (none) |
| 2 | COBS decodes (no `0x00` inside, no group past the end) | `cobs` |
| 3 | At least 7 bytes (header, sequence, CRC) | `length` |
| 4 | CRC matches | `crc` |
| 5 | Version = 1 | `version` |
| 6 | Type is 1, 2 or 3 | `type` |
| 7 | Protobuf decodes | `protobuf` |

- **Joining mid-frame:** the bytes before the first `0x00` are kept if they decode, and dropped silently if not (the tail of a frame is not a link error).
- **Unknown Protobuf fields are ignored,** so a newer firmware works with an older dashboard (tested with an added field 99).

**Sequence accounting**, per MCU1 boot ("session"):

| The PC sees | Meaning |
|---|---|
| The next number | Normal |
| A gap | `missing` += numbers skipped |
| `frames_skipped` grew | `replaced`: MCU1 made those frames but never sent them (busy link) |
| `missing − replaced` > 0 | `lost`: frames that left MCU1 but never arrived intact (noise, baud, cable, adapter, a slow PC) |
| The number went back (or repeated) | MCU1 restarted: a new session starts |

A UART cannot resend, so this does not recover anything. It makes every loss visible and explained, which a one-way link otherwise hides. A lost snapshot is repaired by the next one; a lost event is a permanent hole in the event log. The dashboard shows "Frames received", "Lost on the way" (amber when non-zero), "Replaced by MCU1", "Frame errors" by reason, and "MCU1 restarts seen".

**Uptime.** `uptime_ms` wraps after 2^32 ms (49.7 days). The PC keeps the total (`uptime.py`):
- the sequence went back: MCU1 restarted;
- the uptime went down while the sequence went on: a wrap (+2^32), if the forward step fits the frames in between (at most 100 ms per frame, plus 1 s).

## 8. Link loss

| State | When |
|---|---|
| DISCONNECTED | No byte source: Renode's port not open, or the USB-UART adapter unplugged. Retried every 1 s |
| LOST | Source open, but no **valid** frame for 300 ms (3 snapshot periods). Bytes that fail the checks do not count |
| CONNECTED | A valid frame within the last 300 ms |

- **On LOST or DISCONNECTED** the dashboard keeps the last data, greyed, with "LOST: no valid frame for n s". The health label shows NO DATA. No value is ever substituted or shown as live.
- **On MCU1's restart** (the sequence went back): "MCU1 restarts seen" + 1. A CONFIG arrives within 1 s, and the event log marks where MCU1's times start again at 0.
- **In Renode** the dashboard runs with `--lost-ms 1500`: Renode runs at about 0.3× real time, so snapshots reach the PC about every 0.4 s.
- MCU1 does not know whether a PC is listening; it always sends.

## 9. Versioning

- The header's version (1) changes only if the frame or the meaning of a field changes incompatibly. A PC rejects other versions (`version`).
- Within version 1, fields are only added, with new numbers. Numbers are never reused or renumbered (`snapshots_skipped` was renamed `frames_skipped` and kept field 4).
- After any schema change, run `scripts/gen_proto.sh` (nanopb 0.4.9.1 and `grpc_tools.protoc`, from the uv project in `dashboard/`) and commit the generated files.

## 10. Decisions and rejected options

| Decision | Chosen | Rejected, and why |
|---|---|---|
| Payload | **Protobuf**, one schema; nanopb on MCU1 (Het, 12 Nov 2026) | *Hand-packed fixed layout* (Design Document v0.3): smaller (≈300 B against ≈437 B), but C packing and Python unpacking must be kept equal by hand, and any change breaks an older dashboard. *Text/JSON lines:* several times larger, float formatting and parsing cost on the MCU, no integrity check. *CBOR/MessagePack:* no schema, usually `malloc`, no maximum size known at compile time. *protobuf-c:* needs `malloc`. (The Design Document had listed Protobuf as future scope.) |
| Framing | **COBS + `0x00`** | *Sync header + length:* the sync bytes can occur inside data, so finding the next frame after an error needs a search confirmed by the CRC. *SLIP:* escaping can double a frame, so buffers cannot be sized tightly. *Length prefix only:* one corrupted length byte loses sync. |
| Integrity | **CRC-16/CCITT-FALSE**, the CAN function | *Parity:* catches only odd numbers of bit errors per byte, costs 10 % of throughput (11 bits per byte), and PC drivers report it inconsistently. *No check:* UART noise would reach the screen. One tested CRC function serves both links. |
| Baud | **230,400** | *115,200:* the ≈4,900 B/s measured would be ≈42 % of its 11,520 B/s, over the 30 % ceiling. *460,800, 921,600:* not needed at ≈21 % load. A lower rate keeps more margin for clock error, cables and adapters. |
| Flow control | **None** | *RTS/CTS:* the link is one-way, the PC reads far faster than 23 KB/s, and MCU1 never waits anyway. |
| After an error | **Drop the frame**; the next snapshot (≤100 ms) restores the view | *Retransmission:* needs a return channel and buffering on MCU1, and the resent data is older than the next snapshot. |
| Messages | **SNAPSHOT + EVENT + CONFIG** | *Snapshots only:* a state shorter than 100 ms would never be seen. *Delta snapshots:* one lost frame corrupts the view until the next full one. |
| Busy link | **Latest wins**, two buffers per kind, event queue (Het, 12 Nov 2026) | *Ring buffer with a 256 B reserve* (Design Document §8.4): after a stall it sends a backlog of old snapshots; latest wins always sends the newest and suits a DMA. |
| Number of a frame never sent | **Used up and counted** (`frames_skipped`) | *Reused:* replaced frames would be invisible, and gaps could not be explained. |
| Finding changes for events | **Change counts, a look after every possible change** (Het, 13 Nov 2026) | *Hooks in diag/ and params/:* they would call monitor/, against the module rule (Design Document §4.4). *Sampling every 5 ms* (built 12 Nov): it missed MCU1's 6 ms RESYNCING at boot in S2. |
| Event size | **Grouped:** one event per kind of change, bit mask | *One per parameter or node:* five frames when one node drops off, although its parameters change together. |
| Health | **The weaker** of online/4 and synced/20 (Het, 12 Nov 2026) | *Average:* 4/4 online with 0/20 synchronised would show 50 % for a network that delivers no data. |
| Online count | **ONLINE or LATE** (Het, 13 Nov 2026) | *ONLINE only:* a late heartbeat dropped health from 100 % to 75 % even while the node's data kept arriving, though one late heartbeat is not yet a failure. |
| Uptime | **uint32 ms, unwrapped on the PC** (Het, 12 Nov 2026) | *Seconds on the MCU:* loses the ms resolution event times need. |
| Debug text | **Separate UART** (USART1) | *Inside this protocol:* log volume would use the link's budget and mix two purposes. |

## 11. Verification

| Level | Test | Shows |
|---|---|---|
| C, on the PC | `firmware/tests/host/test_monitor.c` (`MONITOR PASS`) | COBS (known vectors, 254/255-byte groups, 500 random round trips, no `0x00` out); every snapshot field decoded back with nanopb; one parameter's bytes checked by hand; CONFIG; latest wins on a busy link (`frames_skipped` = 2); the health table and online rule; events in order, one grouped event for two nodes, a full queue (16 kept, 4 refused, the gap explained); worst-case snapshot 759 B, under the 1,046 B maximum |
| C → Python | `dashboard/tests/test_framing.py` | The Python decoder reads the C test's frames and finds the same values; an unknown field 99 is skipped |
| PC link | `dashboard/tests/test_link.py`, `test_uptime.py` | Link states on a fake clock, sessions, missing = replaced + lost; the uptime wrap, and a restart the PC never heard not taken for a wrap |
| End to end, Renode | `renode/check_uart_link.sh s1`…`s4` | Every frame decoded from TCP 3456 while the scenario runs; lost = 0; each snapshot equal to MCU1's debug report of the same pass; events replayed between snapshots give exactly the next snapshot; every change in MCU1's log has its event within 2 ms (measured 0–1 ms), and the other way round; S4: the link LOST once and one MCU1 restart seen |

Results: host tests and pytest pass at commit fab7edb. `check_uart_link.sh` S1–S4 passed on 13 Nov 2026, before the online count included LATE (fab7edb); a rerun is pending.

## 12. Limitations and open items

- **Renode does not pace the UART:** bytes leave at once, so a busy link (a snapshot replaced, the event queue full) cannot happen there. Only the host tests cover it; on the board, `ev_peak` in MCU1's report will show whether 16 queue entries are enough.
- **Encode time** was measured only in Renode (≈0.9 ms per snapshot at its fixed 100 MIPS); the 480 MHz chip should be several times faster. To be measured on the board.
- **Varints grow:** uptime and sequence numbers take more bytes as they grow (uptime: 4 bytes after 35 minutes, 5 after 3.1 days), so a snapshot grows by a few bytes over days. Open: `fixed32` for those fields.
- **Port 3456** (Renode's UART terminal) listens on every network interface. It is read-only, but anyone on the same network could read the stream: use a firewall on untrusted networks.
- **Bytes sent before the PC connects** are dropped by Renode. This is harmless: CONFIG repeats every second, snapshots every 100 ms.
- **The 300 ms loss limit** applies on the board; Renode needs 1500 ms (section 8).
