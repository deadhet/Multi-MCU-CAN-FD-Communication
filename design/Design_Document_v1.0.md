---
title: "Multi-MCU CAN FD Communication and Monitoring — Design Document"
subtitle: "Noccarc Robotics · Embedded Firmware Assignment"
author: "Het"
date: "16 November 2026 · Version 1.0 (final)"
---

# 1. Purpose and status

This document is the specification of the four-MCU CAN FD system: what the system must do, what was decided, why, and which alternatives were rejected. Version 1.0 is the final version. It matches the submitted code; the changes since version 0.3 are listed in section 16.

| Area | Status |
|---|---|
| Platform, firmware execution, CAN FD protocol, parameter database | Final |
| Health, fault detection, recovery | Final |
| UART link (Protobuf payload), host-side rules | Final |
| Dashboard | Final |
| Renode topology, fault scenarios, automated tests | Final |

**Method.** Every design point was handled the same way: requirement → the actual question → options with trade-offs → decision with a recorded reason. Values that the assignment leaves to the candidate (periods, timeouts, baud rate) are derived from calculations rather than chosen by feel. Where the hiring manager clarified a requirement, the design was recalculated from that input.

**What is in this document and what is not.** This document states requirements, decisions, constraints, interfaces and the reason for each. Section 2.1 is the specification sheet: every configured value in one place. Register values, measurements and test results are kept in the repository, next to the code they describe:

| Topic | Repository document |
|---|---|
| Code flow from `main()` to the drivers | `docs/architecture_flow.md` |
| FDCAN driver: clocks, bit-timing registers, filters, message RAM | `docs/fdcan_design.md` |
| Frame layouts, validation counters, restart handling, overload results | `docs/comm_design.md` |
| Parameter table, database rules, settle rule | `docs/params_design.md` |
| Link states, own state, scenario results | `docs/diag_design.md` |
| Scheduler and its timing measurements | `docs/sched_design.md` |
| UART link: fields, worked hex example, measured sizes | `docs/uart_protocol.md` |
| Dashboard | `docs/dashboard_design.md` |
| Fault and break tests | `docs/fault_testing.md` |
| Running the simulation | `docs/running_in_renode.md` |
| Tool versions and Renode model findings | `docs/setup_findings.md` |

# 2. System overview

Four STM32H743VIT6 controllers share one CAN FD bus. Every frame is broadcast; there are no destination addresses. Each controller owns five parameters and keeps a database of all twenty.

| Controller | Role | Owns | Extra duties |
|---|---|---|---|
| MCU1 | Master | P1–P5 | Aggregates network health; sends database and health over the UART link |
| MCU2 | Slave 1 | P6–P10 | — |
| MCU3 | Slave 2 | P11–P15 | — |
| MCU4 | Slave 3 | P16–P20 | — |

```
  MCU1 (Master)    MCU2         MCU3         MCU4
      |             |            |            |
======+=============+============+============+======   CAN FD bus (Renode CAN hub)
      |
      +-- UART link --> Python dashboard --> browser
```

The master is not a gateway: slaves hear each other directly. "Master" only means it also monitors health and talks to the host.

## 2.1 Specification sheet

Every configured value of the system. The section that explains each value is given in the last column.

**Hardware and clocks**

| Item | Value | Section |
|---|---|---|
| Controller | STM32H743VIT6, Arm Cortex-M7 with FPU; 4 nodes | 2 |
| Memory used | Flash 2 MB at 0x08000000; DTCM 128 KB (data, stack); AXI SRAM 512 KB (UART link buffers) | 4.3 |
| Clock source | HSI, 64 MHz (Renode). A crystal (HSE) is required on real hardware for CAN FD tolerance | 4.2, 13 |
| CPU clock (SYSCLK) | 480 MHz, from PLL1, voltage scale VOS0 | 4.2 |
| AHB and AXI clock | 240 MHz | 4.2 |
| APB clocks (USART, FDCAN bus interface) | 120 MHz | 4.2 |
| FDCAN kernel clock | 80 MHz, from PLL2 | 5.1 |
| System tick | SysTick, 1 ms | 4.2 |
| Pins | FDCAN1 RX PD0, TX PD1; USART1 TX PA9 (debug); USART3 TX PD8 (UART link, MCU1) | 5.1, 8.1 |
| Interrupt priorities | FDCAN1 receive 1 (highest), SysTick 2, USART1 and USART3 transmit 3 | 4.3 |

**CAN FD bus**

| Item | Value | Section |
|---|---|---|
| Frame type | CAN FD, 11-bit identifiers, bit rate switching on every frame | 5.1 |
| Arbitration bit rate | 500 kbit/s, sample point 80 % | 5.1 |
| Data bit rate | 2 Mbit/s, sample point 75 %, transmitter delay compensation on | 5.1 |
| Identifiers | <code>(type &lt;&lt; 4) &#124; node</code>: parameter block 0x021–0x024, heartbeat 0x031–0x034 | 5.2 |
| Parameter block | 32 bytes, every 10 ms per node | 5.3, 5.8 |
| Heartbeat | 16 bytes, every 100 ms per node, the first at boot | 5.3, 5.8 |
| Integrity | CRC-16/CCITT-FALSE over CAN ID and payload; protocol version 1; source byte; uint32 sequence | 5.5–5.7 |
| Hardware queues | Rx FIFO 0: 16 frames, blocking; Tx FIFO: 4 frames | 4.3 |
| Software receive queues | 4 queues × 8 frames, newest kept: one for heartbeats, one per peer | 4.3 |
| Bus load | ≈10.5 % worst case; ceiling 30 % | 5.8 |

**Parameters and health**

| Item | Value | Section |
|---|---|---|
| Parameters | 20, five per node, each a 4-byte slot: float32, float32, int32, uint32, bool | 5.4, 6.3 |
| Sample periods (per slot) | 10, 50, 100, 500, 1000 ms | 6.3 |
| Transport stale | No accepted block for more than 30 ms (3 periods) | 6.2 |
| Source stale | Owner's sample older than 3 × its sample period | 6.3 |
| Settle rule after a gap | FRESH again only on a block 5–30 ms after the previous one | 6.2 |
| Node LATE / OFFLINE | 150 ms / 350 ms without a heartbeat (1 / 3 missed) | 7.2 |
| Node DATA_INVALID | Heartbeats arrive, no accepted parameter block for 350 ms | 7.2 |
| Restart slack for uptime wrap | 1 s | 7.3 |
| Network health | The weaker of controllers online out of 4 and parameters synchronised out of 20, in percent | 8.3 |

**Firmware tasks**

| Task | Period | Nodes | Section |
|---|---|---|---|
| Send parameter block (and sample own parameters) | 10 ms | All | 4.1 |
| Freshness; health (diag) | 5 ms each | All | 4.1 |
| Send heartbeat | 100 ms | All | 4.1 |
| Debug report | 1 s | All | 8.6 |
| UART snapshot / configuration | 100 ms / 1 s | MCU1 | 8.3 |

**UART link, debug log and host**

| Item | Value | Section |
|---|---|---|
| UART link | USART3, MCU1 only, transmit only: 230,400 baud, 8N1, no flow control | 8.1 |
| Frame | `[version and type][uint32 sequence][Protobuf payload][CRC-16]`, COBS encoded, 0x00 terminator | 8.2 |
| Messages | SNAPSHOT every 100 ms; EVENT on every change; CONFIG at start-up and every 1 s | 8.3 |
| Largest frame on the wire | SNAPSHOT 1,059 bytes; CONFIG 819 bytes; EVENT 25 bytes | 8.3 |
| Link load | ≈21 % in normal operation; ceiling 30 % | 8.1 |
| Event queue | 16 events | 8.4 |
| Host link-loss timeout | 300 ms without a valid frame | 8.5 |
| Debug log | USART1 on every node: 115,200 baud, 8N1, text, 2 KB transmit buffer | 8.6 |
| Dashboard | Web page at `http://127.0.0.1:8765`, updated 5 times a second | 8.7 |

**Tools**

| Item | Value | Section |
|---|---|---|
| Firmware | Bare-metal C, no RTOS, no dynamic memory, register-level drivers (CMSIS headers only) | 3 |
| Build | Arm GNU Toolchain 15.2, CMake 3.21 or newer; four images from one source tree | 3 |
| Simulation | Renode 1.17.0, one process, one CAN hub, sync quantum 100 µs | 9 |
| Host | Ubuntu 22.04; Python 3.12 managed by uv; packages `protobuf`, `pyserial` | 3, 8.7 |

# 3. Development platform

| Decision | Choice | Reason |
|---|---|---|
| Host OS | Native Ubuntu 22.04, terminal workflow | Required Linux platform; Renode and toolchain run natively |
| Toolchain | Arm's official GNU toolchain, CMake | Ubuntu 22.04's packaged compiler is outdated |
| Driver layer | Fully register-level, CMSIS headers only | Full control and explainability; HAL hides behaviour and its clock setup may hang in Renode |
| HAL usage | Reading reference only | Check register sequences against ST's implementation |
| Third-party code | CMSIS core, `stm32h743xx.h` and nanopb, vendored with licence files | Repository builds offline from a plain clone |
| Images | One source tree, four images `node1.elf` … `node4.elf`; CMake passes `NODE_ID` | Matches "four separate firmware images"; nodes differ by one value |
| Host Python | uv with a locked environment | Reproducible; the system Python is not touched |

The HAL FDCAN driver was kept as a timeboxed fallback in case two nodes did not exchange frames by the end of day 2. It was not needed.

**Repository layout**

```
firmware/
  common/
    drivers/        clock, systick, cycles, fdcan, frame_queue, uart (debug), uart_link, tx_slots,
                    board.c (all pin and clock setup)
    comm/           CAN frame packing, IDs, validation pipeline, restart rule
    params/         parameter database
    diag/           link state per peer, own state, restarts
    monitor/        UART link protocol: Protobuf, COBS, events (linked into node1 only)
    sched/          cooperative scheduler and its timing measurements
    util/           CRC-16
    system_table.c  P1–P20 definitions (owner, name, type, range, sample period)
  nodes/
    master/app_master.c   -> node1.elf
    slave/app_slave.c     -> node2.elf, node3.elf, node4.elf
  startup/, linker/       start-up code, linker script
  third_party/            CMSIS, nanopb (vendored)
  tests/host/             unit tests run on the PC
proto/              monitor.proto: the UART payload, single source for C and Python
dashboard/          Python web dashboard and its tests
renode/             node.repl, system.resc, scenarios, fault benches, terminals
docs/               repository documents (section 1)
```

Every node must know the owner of every parameter (the database stores a source MCU per entry), so ownership lives in the shared `system_table.c`, not per node. All pin and clock setup is isolated in `board.c`, so moving to real hardware is a single-file change.

# 4. Firmware execution model

## 4.1 Scheduler

A table-driven cooperative scheduler: a fixed array of `{task, period_ms, last_run}`. The main loop runs every task that is due and drains the CAN receive queues on every pass, heartbeat queue first. Tasks are short and never block. All periods are visible in one table.

| Task | Period | First run | Nodes |
|---|---|---|---|
| Sample own parameters and send the parameter block | 10 ms | 10 ms | All |
| Freshness | 5 ms | 5 ms | All |
| Health (diag) | 5 ms | 5 ms | All |
| Heartbeat | 100 ms | 0 ms | All |
| Debug report | 1 s | 5 ms | All |
| UART snapshot | 100 ms | 5 ms | MCU1 |
| UART configuration | 1 s | 0 ms | MCU1 |

The order of the table is the order within a pass. The first heartbeat goes out at boot, before the first parameter block, so peers see a restart before its data (7.3).

**Timing rule.** A task is due when `now − last_run ≥ period`; then `last_run = now`. A task that starts late therefore shifts its later runs, and missed runs are not caught up.

**Measurement.** Each node measures, since boot, every task's longest run, its latest start after it was due and its number of late starts, and the longest main-loop pass, and prints them once a second. The timing rule stays until the measurements show lateness that matters; fixed-rate timing (`due += period`) is the recorded alternative (`docs/sched_design.md`).

## 4.2 Clocks and timebase

- **System clock.** The core runs at 480 MHz from PLL1, with AHB and AXI at 240 MHz and all APB buses at 120 MHz. The FDCAN kernel clock is 80 MHz from PLL2. The source is the internal 64 MHz oscillator (HSI) in Renode. On real hardware a crystal (HSE) is required: HSI drifts by about ±1 %, which is outside the CAN FD oscillator tolerance (section 13, Q3).
- **Timebase.** SysTick at 1 ms increments a `volatile uint32_t` millisecond counter, read only through `systick_now_ms()`. A 32-bit read is a single instruction on Cortex-M7 (atomic), so no lock is needed.
- **Wrap safety.** All time checks use the form `(now − last) >= period` with unsigned subtraction; the counter wraps after 49.7 days without error.
- **Cycle counter.** The DWT cycle counter is used only for the timing measurements of 4.1.

## 4.3 Interrupt and main-loop data sharing

Interrupts capture events; the main loop does the work.

| Interrupt | Job | Priority |
|---|---|---|
| FDCAN1 receive | Copy frame and arrival time into the RAM queue chosen by its ID | 1 (highest) |
| SysTick | Increment the millisecond counter | 2 |
| USART1, USART3 transmit | Send the next byte | 3 (lowest) |

**CAN receive queues.** 4 queues × 8 slots of `can_rx_slot_t` (76 bytes: `id`, `rx_time_ms`, `len`, flags [FD, BRS], padding, `data[64]`), about 2.4 KB in total.

- Queue 0: all heartbeats, plus frames carrying this node's own IDs (only possible with a duplicate node).
- Queues 1–3: one per peer, for that peer's parameter blocks.

Rules:

1. Exactly one writer (the ISR) and one reader (the main loop) per queue. The reader never interrupts the writer.
2. The writer fills the slot, executes a memory barrier, then advances the head index.
3. The reader copies the slot to a local variable, then reads the head index again. If the writer has lapped the reader during the copy, the copy is discarded and retried from the oldest intact frame.
4. A full queue overwrites its oldest frame. The ISR never checks for full; the reader detects the lap, counts and skips the lost frames. Nothing blocks and no interrupt is masked.
5. The queues are placed in DTCM, which is not cached, avoiding Cortex-M7 cache coherency issues.

Accepted limit: this node's own IDs share queue 0 with the heartbeats, so a duplicate node could displace heartbeats during a stall. A duplicate node is reported by its own counter (5.7).

**Hardware Rx FIFO.** FDCAN Rx FIFO 0 holds 16 frames and runs in blocking mode. It only fills if the ISR cannot run, for example while the CPU is halted; then the oldest 16 frames are kept and newer ones are refused by the hardware and counted.

**Why per-sender queues that keep the newest frames.** Parameter blocks are periodic: the newest frame is the one that matters, and an older one would only make the database look staler than it is. Separating senders means a node flooding the bus can only overwrite its own queue, and heartbeats never compete with data. The overload measurements that led to this design are in `docs/comm_design.md`.

**CAN transmit.** Sending never waits. A frame goes into the 4-frame hardware Tx FIFO; frames of one sender leave in the order queued. When the FIFO is full, the frame is not sent, it is counted, and its sequence number is not used. No transmit interrupt is used.

**UART transmit.**

- Debug log (USART1, every node): a 2 KB ring filled by the main loop and emptied by the transmit interrupt. Text that does not fit is dropped whole and counted.
- UART link (USART3, MCU1): two buffers per message kind and an event queue, handed to the transmit interrupt by one atomic exchange (8.4).

**Memory placement.** DTCM holds all data and the stack. Buffers that are always written before they are read (the UART link's transmit buffers) live in a no-init section in AXI SRAM: start-up does not clear them, which keeps MCU1's boot time close to the slaves', and AXI SRAM is reachable by DMA, which DTCM is not.

The parameter database is touched only from the main loop, and ownership guarantees that local and received parameters never share entries, so it needs no locking.

## 4.4 Module boundaries

```
nodes/ (app_master, app_slave)         application: wires the services, owns the task table
   |
comm/  params/  diag/  monitor/  sched/    services
   |
drivers/                               the only layer that touches registers
```

1. Calls only go downward.
2. `comm/` is the only module that knows the CAN frame format; `monitor/` is the only one that knows the UART format.
3. Services do not call each other; the application copies data between them. Each service can therefore be unit-tested on a PC without hardware.
4. No shared global variables; modules expose functions through their headers.
5. Static memory only: every buffer and queue is sized at compile time.

# 5. CAN FD protocol

## 5.1 Bus configuration

| Item | Value |
|---|---|
| Frame type | CAN FD, 11-bit identifiers |
| Arbitration (nominal) bit rate | 500 kbit/s, sample point 80 % |
| Data-phase bit rate | 2 Mbit/s, sample point 75 % |
| Bit Rate Switching | Enabled on every frame |
| Transmitter delay compensation | Enabled |

The FDCAN kernel clock is 80 MHz, so both bit rates have an integer number of time quanta. The build stops if the bit rates no longer follow from the clock. The derivation and the register values are in `docs/fdcan_design.md`.

## 5.2 Identifier allocation

`ID = (type << 4) | node`. Lower ID wins arbitration. Priorities are rate-monotonic: the faster message gets the higher priority, which matches standard CAN response-time analysis.

| ID | Message | Period |
|---|---|---|
| 0x011–0x014 | Resync request | Reserved, unused (future scope) |
| 0x021–0x024 | Parameter block | 10 ms |
| 0x031–0x034 | Heartbeat | 100 ms (slowest message) |

The low nibble is always the sender, so every ID shows its sender and no two nodes ever transmit the same ID. Gaps are left for future message types.

## 5.3 Frame layouts

All fields are serialised explicitly in little-endian order with helper functions; structures are never copied directly onto the wire.

**Parameter block (32 bytes)**

| Bytes | Field | Notes |
|---|---|---|
| 0 | Protocol version | 1 |
| 1 | Source node | Must equal the ID's low nibble |
| 2–3 | Status word | Bits 0–9: 2-bit source status per parameter (0 OK, 1 SOURCE_STALE, 2 SOURCE_FAULT, 3 reserved). Bits 10–15 reserved |
| 4–7 | Sequence | uint32 |
| 8–27 | Five values | 4 bytes each, type from `system_table.c` |
| 28–29 | CRC-16 | See 5.5 |
| 30–31 | Padding | 0x00 (the next valid CAN FD size above 30 bytes is 32) |

**Heartbeat (16 bytes)**

| Bytes | Field | Notes |
|---|---|---|
| 0 | Protocol version | 1 |
| 1 | Source node | Cross-check against ID |
| 2 | Node state | BOOT / RESYNCING / OPERATIONAL (7.4) |
| 3 | Synced count | Parameters this node sees as valid and fresh (0–20) |
| 4–7 | Heartbeat sequence | uint32 |
| 8–11 | Uptime (ms) | uint32; restart detection (7.3) |
| 12–13 | Error summary | Saturating sum of the node's error counters (5.7) |
| 14–15 | CRC-16 | See 5.5 |

## 5.4 Data types

Every value occupies a fixed 4-byte slot. Its type (float32, int32, uint32 or bool) and its valid range come from `system_table.c`. Parameters use generic names ("Param 1" … "Param 20").

The table defines a minimum and maximum per parameter. For now each range is the type's full domain, so the range check (5.7, check 8) rejects only what can never be a value: a float NaN or infinity, and a bool above 1. Tighter limits are a table change only.

## 5.5 End-to-end CRC with Data ID

- **Algorithm:** CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR. Check value: `"123456789"` → 0x29B1. Each node verifies the check value at start-up and does not start CAN if it fails.
- **Coverage:** `[CAN ID as 2 bytes, little-endian] + every payload byte before the CRC`. The ID acts as an implicit Data ID: it is included in the calculation but not transmitted again.
- **Placement:** the sender computes it as the last packing step; the receiver checks it on the exact local copy it then decodes. This makes the protected window as wide as possible.

**Justification.** The design follows the black channel principle of IEC 61784-3: the bus, controllers, drivers and buffers are treated as untrusted, and the end applications protect the data. Each error class has a countermeasure:

| Communication error (IEC 61784-3) | Countermeasure in this design |
|---|---|
| Repetition, incorrect sequence, loss | Sequence counter |
| Delay | Freshness threshold, settle rule, heartbeat timeouts |
| Insertion, addressing | ID scheme, source-byte check, duplicate-node detection, Data ID in CRC |
| Corruption | CRC-16 (the only countermeasure for this class) |

The CAN bus CRC protects only the wire. The application CRC acts as a seal from packing to decoding and also detects corruption inside a node (controller, message RAM, buffers). The Data ID detects a valid payload paired with the wrong identifier. The cost is 2 bytes per frame, taken from existing padding, and a CPU-time budget of a few microseconds per frame.

**Known limits.** The CRC does not identify the cause of an error, does not detect errors made before sealing or after checking, and provides no security. Errors before sealing are covered by host unit tests and by range checks.

CRC-16 was chosen over CRC-8 because it is 256 times stronger against random corruption (1 in 65,536 instead of 1 in 256), detects all bursts up to 16 bits, and detects every odd number of bit errors, at no extra frame size.

## 5.6 Sequence counters

- uint32, incremented on every frame that is actually queued for transmission (it counts frames, not value changes). It wraps after about 497 days at 10 ms.
- Receiver rule: `d = (int32_t)(seq − last)`. `d > 0`: accept, and count `d − 1` lost frames; `d == 0`: duplicate, reject; `d < 0`: old, reject, except a restart heartbeat (7.3).
- Each sender starts in a NEVER_HEARD state; the first frame is always accepted. This also makes a simultaneous power-up of all nodes a normal cold start.
- Tracking resets to NEVER_HEARD when a restart is detected and when the sender goes OFFLINE, so any return (reset, halt or disconnection) is accepted cleanly.

## 5.7 Validation pipeline

Checks run in order, cheapest first, stopping at the first failure:

| # | Check | Failure handling |
|---|---|---|
| 0 | Hardware acceptance filters; bus CRC. The same ID range list routes each accepted frame to its RAM queue | Dropped by hardware |
| 1 | Known ID, sender 1–4, not this node | Drop + count (own ID from another node counts as `duplicate_node`) |
| 2 | FD frame with BRS set | Drop + count |
| 3 | Length matches message type | Drop + count |
| 4 | Version = 1 | Drop + count |
| 5 | Source byte = ID low nibble | Drop + count |
| 6 | CRC-16 with Data ID | Drop + count |
| 7 | Sequence rule (5.6) | Duplicate or old: drop + count. Gap: count lost frames, accept. Restart heartbeat (7.3): accept |
| 8 | Per value: source status, NaN/Inf, min/max | Only that parameter is marked INVALID; the others are accepted |

Two orderings are essential: length before CRC (the CRC position depends on the length) and CRC before sequence (a corrupted sequence number would poison the last-seen value).

**Golden rule:** a rejected frame never changes the database. The old value stays, ages and becomes stale, so a problem is always visible.

**Error counters (per node).** `comm/` counts each rejection by reason: `bad_id`, `duplicate_node`, `bad_format`, `bad_length`, `bad_version`, `bad_source`, `bad_crc`, `duplicate`, `old_seq`, and `lost_frames`. `rx_overflow` is the sum of the driver's counters: frames the hardware FIFO refused and frames overwritten in each RAM queue. `params/` counts `value_rejected` (check 8 failures in received blocks). The driver also counts Tx-full events. All counters appear in every node's debug report. Each heartbeat carries their saturating sum as the error summary, and MCU1 forwards each node's summary over the UART link.

Bus-off detection and recovery are not implemented: physical bus errors are not modelled by Renode (section 12).

## 5.8 Transmission timing and bus load

**Requirement (hiring manager):** the heartbeat is the slowest message at 100 ms; every other periodic message must be faster, with rates derived from the configured bit rates.

**Frame duration.** The arbitration-phase portion (SOF to BRS, then CRC delimiter, ACK, EOF and interframe space) is about 30 bits at 2 µs each. The data-phase portion (ESI, DLC, data, stuff count, CRC and fixed stuff bits) runs at 0.5 µs per bit. Dynamic stuffing adds at most about 1 bit per 4. Worst-case values are used.

| Frame | Payload | Typical | Worst case |
|---|---|---|---|
| Heartbeat | 16 B | ≈140 µs | ≈165 µs |
| Parameter block | 32 B | ≈207 µs | ≈247 µs |

Bus load (4 nodes; heartbeats add 0.66 %):

| Parameter period | Total worst-case load |
|---|---|
| 2 ms | ≈50 % |
| 5 ms | ≈20 % |
| 10 ms (chosen) | ≈10.5 % |
| 20 ms | ≈5.6 % |

**Decision:** parameter block every 10 ms, heartbeat every 100 ms, bus-load ceiling 30 %. 10 ms is ten times faster than the heartbeat, uses about one third of the budget, keeps per-node receive load at about 300 frames/s, and is practical in Renode. Headroom covers retransmissions, recovery bursts and future messages.

**Heartbeat worst-case delay** (lowest priority): one frame already on the bus (247 µs) + four parameter frames (988 µs) + three lower-ID heartbeats (495 µs) + itself (165 µs) ≈ 1.9 ms, against a 100 ms period. Delays never accumulate between heartbeats.

No transmission staggering is used: the nodes share no clock, so offsets would be meaningless. Simultaneous starts are resolved by CAN arbitration in microseconds, without loss.

# 6. Parameter database

## 6.1 Structure

- **Static part** (flash, `const`, shared table): ID, owner, name, type, min, max, sample period, source-stale limit.
- **Live part** (RAM, private to `params/`): one entry per parameter with value, sequence, time of last update, validity, freshness and an owner-offline flag.
- The owner-offline flag is set while the parameter's owner is OFFLINE (from `diag/`, copied by the application). Values, validity and freshness are unchanged by it.
- The node's own five parameters live in the same database.
- Other modules read a copy, never a pointer.
- Age is never stored; it is computed as `now − last_update_ms`.

## 6.2 Validity and freshness

They are independent questions: can the content be trusted, and is it recent.

| Status | States | Rule |
|---|---|---|
| Validity | NEVER_RECEIVED, VALID, INVALID | INVALID if the owner reports SOURCE_FAULT, the status is the reserved code, or check 8 fails. The old value is kept but flagged |
| Freshness | FRESH, STALE | STALE if no accepted block for more than 30 ms (3 × period, tolerates two lost frames), or the owner reports SOURCE_STALE, or the sender is settling (below) |

**Synchronised = VALID and FRESH.** Only synchronised values count as current.

- An INVALID parameter still takes the new block's sequence and arrival time, because the frame did arrive. It therefore shows INVALID + FRESH while blocks keep coming, and is not synchronised.
- The owner-offline flag (6.1) is reported alongside validity and freshness but does not change either of them.

**Two kinds of staleness** are covered: transport stale (frames stopped arriving, detected by the receiver) and source stale (frames still arrive but the owner's sample is old, reported by the owner through the status word). The owner sends a status flag rather than a timestamp, because node clocks are not synchronised and an absolute timestamp from another node would be meaningless.

**Settle rule.** After a sender's blocks were missing for more than 30 ms, its parameters become FRESH again only when a block arrives 5–30 ms after that sender's previous block. Blocks arriving before that are applied but leave the parameters STALE. The first block ever from a sender is trusted at once.

Reason: frames left in a sender's Tx FIFO by a disconnection go out back-to-back at reconnect, with values from before it. They pass checks 1–8 and carry a new arrival time, so freshness by arrival time alone would show old values as current. A block on the normal 10 ms beat can only have been sampled after the reconnect. The rule fails towards STALE. Accepted limit: a link that drops again 5–30 ms into the flush could release one old block as FRESH.

A freshness task runs every 5 ms, updates the stored flags and logs FRESH↔STALE changes. Per sender, the arrival gap between blocks is also tracked (last gap, worst gap) as evidence that the 10 ms period holds.

## 6.3 Parameter generation

- Every node uses the same 5-slot layout: float32 sampled every 10 ms, float32 every 50 ms, int32 every 100 ms, uint32 every 500 ms, bool every 1000 ms. Each type and each period therefore runs on all four nodes.
- Values follow a pattern by type: float → sine, integer → ramp, bool → toggle.
- The owner samples a parameter in the parameter send task when its sample period has passed, so a block always carries the newest samples. Sampling rate and bus rate are separate: fast control loops stay inside the owning MCU, and the bus carries supervisory data.
- The owner sets SOURCE_STALE when its own sample is older than 3 × its sample period, and until the first sample. An own value that fails check 8 is sent as SOURCE_FAULT.
- Fault injection at run time (freeze, out of range, NaN, mark faulty) is future scope; the per-parameter status field is ready for it. Corrupted values are tested with build-time mutations (section 9).

# 7. Health, fault detection and recovery

`diag/` detects and reports only. It takes no fault actions: it never stops sending, resets a node or refuses a peer.

## 7.1 Why a separate heartbeat

Parameter frames answer "is this data current?"; heartbeats answer "is this node healthy, and in what state?". The heartbeat carries state, synchronisation progress, uptime and an error summary. Together the two streams distinguish a dead node (both stop), a live node with a broken data path (heartbeat continues) and a live node with a broken heartbeat path (data continues). Data staleness reacts in 30 ms; node offline status reacts in about 350 ms, so a single delayed frame never makes a node flicker offline.

## 7.2 Link state of each peer

Runs on every node (each needs to see restarts in order to reset sequence tracking); the master additionally reports it.

| State | Entered when | Left when |
|---|---|---|
| UNKNOWN | Boot | First heartbeat → ONLINE; 350 ms after boot without one → OFFLINE (or NO_HEARTBEAT) |
| ONLINE | A heartbeat from UNKNOWN, LATE or NO_HEARTBEAT; a confirmed recovery; an accepted block while DATA_INVALID | 150 ms without a heartbeat → LATE; a restart → RECOVERING; no accepted block for 350 ms → DATA_INVALID |
| LATE | ONLINE or DATA_INVALID, and 150 ms without a heartbeat | Heartbeat → ONLINE; 3 missed (350 ms) → OFFLINE or NO_HEARTBEAT |
| OFFLINE | 3 heartbeats missed and its parameters not all VALID and FRESH | Heartbeat → RECOVERING; its parameters arriving properly again first → NO_HEARTBEAT |
| RECOVERING | The first heartbeat after OFFLINE; a restart | A later heartbeat that reports OPERATIONAL → ONLINE; 3 missed → OFFLINE or NO_HEARTBEAT |
| NO_HEARTBEAT | 3 heartbeats missed, but all five of its parameters VALID and FRESH | Heartbeat → ONLINE (RECOVERING if it was OFFLINE before); its data failing too → OFFLINE |
| DATA_INVALID | ONLINE, heartbeats arriving, but no parameter block from it accepted for 350 ms | An accepted block → ONLINE (a heartbeat does not end it); 150 ms without a heartbeat → LATE; a restart → RECOVERING |

**Missed count.** Heartbeats missed since the last one: 1 at 150 ms of silence, +1 every 100 ms after that; OFFLINE at 3 (350 ms). 150 ms is 1.5 × the period, which absorbs the ≈1.9 ms worst-case delay. One lost heartbeat makes a node LATE but never OFFLINE.

**RECOVERING is confirmed only by a later heartbeat.** The first heartbeat after OFFLINE can never also confirm the recovery, even if it reports OPERATIONAL. After a reconnect it may be an old one from the sender's Tx FIFO, still reporting the state from before the outage.

**NO_HEARTBEAT.** A peer whose heartbeats stopped but whose parameters still arrive VALID and FRESH is alive, with only its heartbeat path failing. It is not forgotten and its parameters are not flagged.

**DATA_INVALID**, the mirror of NO_HEARTBEAT. A peer whose heartbeats arrive but whose parameter blocks are all rejected or missing is alive with a broken data path. It is not OFFLINE, and it is not waited for by the other nodes' own state (7.4).

**On OFFLINE** the application resets the sender's sequence tracking (5.6) and sets the owner-offline flag on its parameters (6.1).

## 7.3 Restart detection

- **Rule.** A restart is a heartbeat that passes checks 1–6, has a sequence lower than the last one accepted, and reports BOOT or an uptime lower than the last one. The receiver resets both streams of that sender to NEVER_HEARD, accepts the heartbeat as its first frame and counts the restart.
- **First heartbeat at boot.** A node sends its first heartbeat at boot, before its first parameter block, and it always reports BOOT. Peers therefore see the restart before the node's data.
- **If the boot heartbeat is lost,** the next one still triggers the rule through the lower uptime. If none gets through, the node goes OFFLINE and is forgotten there.
- **Restart during an outage.** A sender that was forgotten on OFFLINE carries no sequence history, so `diag/` also counts a restart when a peer's uptime goes down.
- **Uptime wrap.** The uptime is a uint32 in ms and wraps after 49.7 days. A lower uptime is a restart unless it is a wrap: the step forward through 2^32 exceeds the time passed at the receiver since the previous heartbeat by at most 1 s. The 1 s covers clock rounding, the arrival delay of the previous heartbeat and crystal drift. A higher uptime is never a restart.
- A halted CPU that continues keeps a rising uptime, so it is not counted as a restart.

Reason for the rule: it detects even very fast reboots, and a corrupted frame cannot fake a restart because the CRC is checked first.

## 7.4 Own state and resynchronisation

The periodic 10 ms broadcast is the resynchronisation mechanism; no request message is needed.

| Own state | Entered when | Left when |
|---|---|---|
| BOOT | Reset | A heartbeat from any other node → RESYNCING |
| RESYNCING | After BOOT; or OPERATIONAL with no connected peer left | Every connected peer has sent an accepted parameter block since it was last OFFLINE → OPERATIONAL |
| OPERATIONAL | As above | No peer connected → RESYNCING |

- Connected = ONLINE, LATE, RECOVERING or NO_HEARTBEAT. A DATA_INVALID peer is not waited for.
- A node alone never reports OPERATIONAL.
- Other nodes coming and going do not change a node's own state.
- The synced count in its heartbeat stays honest (for example 15 when one node is offline).
- Accepted limit: if the bus split into two groups of two or more, both could report OPERATIONAL; the debug log shows each node's view of the others.

| Case | Time to OPERATIONAL |
|---|---|
| All nodes alive | About one parameter period after the first heartbeat from a peer |
| One node dead | No extra wait: a dead node is not a connected peer, so its 350 ms timeout is not waited for |

## 7.5 Master failure

Slaves detect the master as OFFLINE with the same state machine, keep exchanging parameters, mark P1–P5 stale and set their owner-offline flag. The host sees the UART link go silent (8.5). No failover is implemented; the behaviour is demonstrated by scenario S4.

# 8. UART link and debug logging

## 8.1 Configuration

USART3 in asynchronous mode (the UART link), master only.

| Item | Value |
|---|---|
| Baud rate | 230,400 |
| Format | 8N1, no parity |
| Flow control | None |
| Direction | Master → host, transmit only |

**Throughput.** With 8N1 each byte costs 10 bits, so 230,400 baud carries 23,040 B/s. Planned traffic is about 4.9 KB/s, a load of about 21 % (under the same 30 % ceiling as CAN). A typical snapshot takes about 19 ms to send, the largest possible one about 46 ms, both inside the 100 ms period.

**Baud accuracy.** `BRR = clock / baud`, rounded: 120 MHz → 521 → −0.03 %, far inside the usual ±2 % tolerance.

Parity is not used: the frame CRC-16 detects every error parity would (all odd numbers of bit errors) plus multi-bit errors and bursts, while parity would cost about 10 % throughput. Flow control is not used: the link is one-way, the host buffers far more than it receives, and the transmit policy (8.4) already guarantees the firmware never blocks.

## 8.2 Framing

```
[version and type][uint32 sequence][Protobuf payload][CRC-16]   -> COBS encode -> 0x00 terminator
```

- **Header byte:** protocol version (1) in the high nibble, message type in the low nibble (1 SNAPSHOT, 2 EVENT, 3 CONFIG).
- **Sequence:** uint32, 1 for the first frame after a reset, +1 per frame made.
- **Payload:** a Protobuf message defined in `proto/monitor.proto`, the single source of the UART protocol for firmware and host. It is encoded on MCU1 with nanopb (static, no dynamic memory) and decoded in Python with the generated module.
- **CRC:** the same CRC-16 as on CAN, over header, sequence and payload.
- **COBS** (Consistent Overhead Byte Stuffing) removes every 0x00 from the frame, so 0x00 can mark frame ends unambiguously. Overhead is at most 1 byte per 254, and after any error the next 0x00 is a true frame end.

COBS, CRC-16 and the sequence number stay outside Protobuf, which provides neither framing nor integrity. All packing lives in `monitor/`.

## 8.3 Message types

| Message | When | Payload, at most | Contents |
|---|---|---|---|
| SNAPSHOT | Every 100 ms | 1,046 B (typically about 440 B) | MCU1's uptime; health of all 4 MCUs; all 20 parameters with typed value, source, validity, freshness, age, sequence, source status, owner-offline flag; frames skipped; online count, synced count, health percent |
| EVENT | Immediately on a change | 16 B | Time, and one of: parameter change with a bit mask of parameters; node link change or restart with a bit mask of nodes; MCU1's own state |
| CONFIG | At start-up and every 1 s | 807 B (typically about 390 B) | Bit rates and BRS from the configuration constants, protocol and firmware version, node and parameter counts, periods, parameter names and owners |

- **Sizes** are the maximums from the Protobuf generator; every buffer on MCU1 is sized from them. Measured sizes are in `docs/uart_protocol.md`.
- **Typed values.** A parameter value is a typed field (float, int32, uint32 or bool), so the host needs no copy of the system table. States are schema enums, with zero always the "not good" value.
- **Snapshots are self-contained:** one good snapshot restores the whole view. Events ensure short transitions are never missed and provide an event log. CONFIG repeats so a host that connects late still receives it.

**Network health.** Health = the weaker of controllers online (MCU1 plus every peer ONLINE or LATE, out of 4) and parameters synchronised (VALID and FRESH at MCU1, out of 20), each in whole percent rounded down. Examples: 4/4 and 20/20 → 100; 3/4 and 15/20 → 75; 4/4 and 0/20 → 0. It is computed on MCU1 and sent in every snapshot. The average of the two was rejected: it would show 50 % when every node talks but no data arrives.

**Event detection.** `diag/` and `params/` each count every change of a state the link reports. MCU1 looks for changes right after anything that can make one (after the task pass and after each CAN frame received), but only when a count moved, and before every snapshot on the snapshot's own data. The events sent before a snapshot therefore explain it exactly. Each kind of change becomes one event naming every node or parameter that made it. Every change is reported, however short.

## 8.4 Transmit policy

MCU1 never waits for the link.

- **Latest wins for SNAPSHOT and CONFIG.** Each kind has two buffers: one may be on its way out, the next frame of that kind is written into the other. A frame still waiting when a newer one of its kind is made is replaced, never queued behind. After a stall the newest data goes out, not a backlog.
- **Events** wait in a 16-entry queue and are never replaced. When the queue is full, a new event is refused; the next snapshot still carries the state.
- **Accounting.** Every frame made gets a sequence number, including one that is replaced or refused. Those are counted in `frames_skipped`, sent in every snapshot, so the host can tell them from frames lost on the wire.
- **Order.** Frames leave in the order they were handed over.
- **Byte mover.** In Renode the USART3 transmit interrupt sends one byte at a time. On hardware a DMA stream can do it with the same buffers.

## 8.5 Host-side handling

- **Validation, in order:** split at 0x00 → COBS decode → length → CRC → version → known type → Protobuf decode. A frame that fails a check is dropped, counted by reason and never shown.
- **Forward compatibility:** fields unknown to the host are ignored, so a newer firmware works with an older dashboard.
- **Sequence accounting:** a gap in the sequence is a replaced frame if `frames_skipped` grew by as much; otherwise it was lost on the way. A sequence that goes back means MCU1 restarted.
- **Link states:** CONNECTED (a valid frame within 300 ms), LOST (source open, none for more than 300 ms) and DISCONNECTED (no byte source; retried every second).
- **On LOST or DISCONNECTED** the last values stay visible but are clearly marked as not current. No value is ever substituted or presented as live.
- **Uptime:** MCU1 sends its uptime as uint32 ms, which wraps after 49.7 days. The host keeps the total: uptime lower and sequence continuing is a wrap; sequence lower is a restart. Date and time of day come from the host's clock; the MCU has no calendar.

## 8.6 Debug logging

Every node uses USART1 (115,200 baud, 8N1) as a plain-text debug log containing a report once a second and a line for every state change, never per-frame output. In Renode these go to `logs/mcuN.log`, which provides the simulation-log deliverable.

- Rejected frames are logged by reason: the first one at once, then at most one line per second per reason, with a count.
- Debug text dropped because the 2 KB buffer was full is counted and printed in the report.

## 8.7 Dashboard

- **Technology.** A web page served by the Python application on 127.0.0.1 only, using the standard library, updated five times a second over Server-Sent Events. Chosen over a desktop GUI toolkit: no extra dependencies and no build step.
- **Layout.** A header (UART link state, system health, MCU1 uptime, PC time) and exactly three sections, as the assignment requires: overall network health, individual MCU health with the event log, and the 20 parameters.
- **Source.** It shows only what MCU1 sends. All decisions are made in Python and unit-tested; the page only draws.
- **Display rules.** Normal states are plain text. Abnormal states are coloured and always carry a word and a shape: amber triangle for caution, red square for fault, blue circle for in transition.
- **Health label.** 100 % HEALTHY, 1–99 % DEGRADED, 0 % DOWN; NO DATA while the UART link is not CONNECTED.

# 9. Renode simulation and verification

| Item | Decision |
|---|---|
| Platform description | A thin `node.repl` that includes Renode's stock STM32H743 platform; any override is minimal and documented |
| Setup script | One `system.resc`: create the CAN hub, create 4 machines, load `nodeN.elf`, connect each `fdcan1` to the hub |
| One process | All four nodes run in one Renode process: one CAN hub and one simulated clock, so nodes stay in step |
| UART routing | Master USART3 → TCP socket (port 3456); every USART1 → `logs/mcuN.log` |
| Sync quantum | 100 µs |
| Scenario timing | `emulation RunFor` between steps, so faults occur at exact virtual times and runs are reproducible |

**Simulation constraints that shape the design** (details in `docs/setup_findings.md`):

- Frames are delivered whole and at once: no bit timing, arbitration timing, error frames or bus-off.
- Writes to the FDCAN bit-timing registers are not kept, so CONFIG reports the bit rates from the configuration constants.
- The UART is not paced by its baud rate, so the busy-link behaviour of 8.4 can only be tested on the PC.
- CPU time is simulated at a fixed rate, so measured run times are Renode's, not the chip's.

**Scenarios**

| Script | Steps | Covers |
|---|---|---|
| S1 Normal | Run 10 s | Normal operation |
| S2 Slave failure | Halt MCU3 at 5 s, reset at 10 s, run 5 s | Assignment scenarios 2–7: a slave stops, the others continue, the master detects it, the slave restarts and resynchronises |
| S3 Disconnect | Disconnect MCU3 at 5 s, reconnect at 15 s (no restart), run 5 s | Sequence reset on OFFLINE, settle rule, recovery without a restart |
| S4 Master failure | Halt MCU1 at 5 s, reset at 10 s, run 5 s | Section 7.5 |

**Verification required** (results in the repository documents):

| Level | What must be shown |
|---|---|
| Unit tests on the PC | Receive queues, parameter database, health states, scheduler, UART transmit buffers, UART protocol; the dashboard's decoding, link states and page model |
| Scenario tests | S1–S4 run and check themselves: every node's log must show the expected events in order |
| UART link end to end | Every frame decoded while a scenario runs and compared with MCU1's own log |
| Fault injection (mutation) | One node's frames deliberately broken, once for every validation check; the receivers must reject or flag each |
| Overload | Main loop stalled, CPU halted, and one node flooding the bus |
| Counter wrap | Millisecond clock and sequence counters crossing 2^32 |

# 10. Timing summary

| Item | Value |
|---|---|
| Scheduler tick | 1 ms |
| Parameter block period | 10 ms |
| Heartbeat period | 100 ms (slowest message) |
| Parameter stale threshold | 30 ms (3 × period) |
| Settle window | A block 5–30 ms after the previous one |
| Freshness task; health task | 5 ms each |
| Node LATE | 150 ms without a heartbeat |
| Node OFFLINE | 3 missed heartbeats (350 ms) |
| Node DATA_INVALID | 350 ms without an accepted parameter block |
| Recovery confirmed | By the second heartbeat after the return, about 100 ms after RECOVERING |
| Resync complete (own state OPERATIONAL) | About one parameter period after the first peer heartbeat; a dead node adds no wait |
| Source-stale limit | 3 × each parameter's sample period |
| CAN bus load | ≈10.5 % worst case (ceiling 30 %) |
| Heartbeat worst-case delay | ≈1.9 ms |
| Rx queue cover | 8 slots ≈ 80 ms of one peer's frames |
| UART snapshot / CONFIG | 100 ms / 1 s |
| UART link load | ≈21 % at 230,400 baud (ceiling 30 %) |
| Host link-loss timeout | 300 ms |
| Dashboard refresh | 200 ms |
| Renode sync quantum | 100 µs |

# 11. Rejected options

**Platform:** WSL2, virtual machine and Docker as primary environment · full ST HAL · HAL for FDCAN only (kept only as timeboxed fallback, not needed) · Git submodules or CMake FetchContent for third-party code · single binary with runtime node ID.

**Execution:** super-loop with tick flags · running tasks inside the timer interrupt · TIM2 or DWT as the system timebase · single-slot buffer with flag · ISR writing directly into the database · polling-only CAN reception · pointer-based database access · fixed-rate task timing (`due += period`; recorded as the alternative, 4.1).

**Receive path:** one shared ring dropping the newest frame · one bigger ring · Rx FIFO overwrite mode · masking interrupts around the queue copy · `LDREX`/`STREX` slot claim · latest-only mailbox per stream · draining heartbeats less often.

**Protocol:** node-first ID layout · one ID per parameter · one ID per node with type in payload · fixed "liveness before data" priority order · 29-bit IDs · one frame per parameter · parameters merged into the heartbeat · packed-struct memcpy serialisation · all-float32 encoding · all-int32 scaled encoding · type tag sent in the frame · uint8 sequence · uint16 sequence · strict `seq == last + 1` acceptance · accept-all sequence handling · silent drop on error · negative acknowledgement to the sender · CRC-8 · CRC-32 · additive or XOR checksums · inverted-copy redundancy · STM32 hardware CRC unit · no end-to-end check · on-change-only transmission · periodic plus on-change transmission · transmission staggering · master SYNC message · 1 ms, 2 ms, 5 ms, 20 ms and 50 ms parameter periods · 50 % bus-load ceiling.

**Database and health:** separate storage for own parameters · stored age · 2 × or 5 × stale thresholds · freshness computed only on read · inter-arrival interval as the freshness measure · reusing the owner-valid mask for staleness · per-parameter source age in the frame · minimal heartbeat · 50 ms and 1000 ms heartbeat periods · two-state online/offline model · restart detection by offline-and-return alone · restart by "sequence backwards" alone (a corrupted or old frame could fake it) · explicit resync request (now future scope) · master failover.

**Old frames after a reconnect:** cancelling them at the sender (the Renode FDCAN model jams on cancel) · a sample timestamp in the frame (no common clock) · skipping a fixed number of blocks (ties the database to the Tx FIFO depth).

**Uptime and restart:** uptime in seconds or as floating point · comparing every heartbeat's uptime step with the time passed (false restart after every reconnect).

**UART link:** text or JSON lines · sync-header binary framing · a fixed hand-written binary layout for the payload (smaller, but C and Python must be kept equal by hand) · CBOR or MessagePack (no schema) · protobuf-c (needs dynamic memory) · delta snapshots · snapshot-only messaging · a transmit ring with a reserve for events (replaced by latest wins) · looking for events on a 5 ms timer (missed shorter states) · the average of the two health figures · 115,200, 460,800 and 921,600 baud · parity · RTS/CTS flow control · retransmission on error · logs carried inside the monitor protocol.

**Simulation and dashboard:** four Renode processes joined by a virtual CAN interface (separate clocks, needs root) · Renode's TCP monitor for control (listens on every interface) · a desktop GUI toolkit for the dashboard.

# 12. Future scope

- **Hardware bring-up:** HSE crystal; DMA for the UART link; measurement of task run times, UART encode time and the transmitter delay on the real chip and transceiver.
- **Watchdog:** a node whose main loop hangs is detected by the others but does not reset itself.
- **Bus-off detection and recovery.**
- **Master failover.**
- **Babbling-node event:** a flooding node is contained by its own receive queue; an event for it is not raised yet.
- **A queue that never overwrites, for command or event messages on CAN,** if such messages are ever added. The keep-newest queues would lose them silently.
- **Explicit resync request** (IDs 0x011–0x014) if parameter periods grow (rate groups, on-change parameters).
- **Rate-group frames** if parameters need different bus rates.
- **Real ranges per parameter** in the system table.
- **Cancelling old Tx frames at the sender** after a reconnect, to remove the cause the settle rule handles.
- **Run-time fault injection** (freeze, out of range, NaN, faulty) through a RAM command block written by Renode.
- **Fixed-rate task timing** if measurements on hardware show lateness that matters.
- **UART link:** fixed-width fields for uptime and sequence so a snapshot keeps one size; delta snapshots with periodic keyframes if bandwidth ever becomes a constraint; repeating recent events inside snapshots if event delivery must be guaranteed; RTS/CTS on real hardware.
- **Dashboard:** trend lines per parameter; acknowledging alarms; saving the event log to a file; choosing the serial port from a list.

# 13. Questions for the hiring manager

| # | Question | Status / working default |
|---|---|---|
| Q1 | Minimum update rate; different rates per parameter? | Answered: heartbeat is the slowest at 100 ms; all other messages faster, derived from the bit rates; one fast rate for all parameters is acceptable |
| Q2 | Are all parameters of fixed size? | Open. Default: every parameter fits in 4 bytes |
| Q3 | Must firmware be ready for real STM32H743VIT6 boards, or is Renode-only enough? | Open. Default: Renode first; the firmware is written for the real chip, hardware specifics are isolated in `board.c`, and an HSE crystal is needed on a board |
| Q4 | Submission format and deadline | Deadline answered: 6 December 2026. Format default: GitHub repository plus a short recording |

# 14. Decision log

| ID | Decision | Key reason |
|---|---|---|
| A1 | Native Ubuntu 22.04 | Required Linux platform |
| A2 | Register-level, CMSIS only; HAL as reference | Control and explainability |
| A3 | CMake, four images from one tree via `NODE_ID`; vendored CMSIS and nanopb | Matches "four images"; offline build |
| B1 | Table-driven cooperative scheduler, `last_run = now` | Deterministic, all periods in one table |
| B2 | SysTick 1 ms, wrap-safe comparisons; core 480 MHz, FDCAN kernel clock 80 MHz | Simple, sufficient resolution; integer time quanta for both bit rates |
| B3 | Per-sender SPSC queues keeping the newest frames (lap detection), DTCM, priorities FDCAN > SysTick > USART | Lock-free, losses counted, one flooding node isolated, no cache issues |
| B4 | Layers with calls only downward; services do not call each other; `comm/` owns the CAN format | Required module separation; testability |
| B5 | Task timing measured on every node and printed each second | Evidence of determinism before changing the timing rule |
| B6 | CAN transmit never waits; a frame that does not fit is counted and uses no sequence number | The main loop never blocks; receivers see no false loss |
| C1 | <code>(type &lt;&lt; 4) &#124; node</code>, 11-bit, rate-monotonic priorities | Unique IDs per sender; standard timing analysis |
| C2 | One 32-byte frame per node, explicit little-endian serialisation | Few frames; compiler-independent layout |
| C3 | 4-byte slots, type and range from table | Uniform layout, deliberate encoding |
| C4 | uint32 sequence, signed-difference rule, NEVER_HEARD, reset on restart and OFFLINE | Robust to loss, wrap, restart and long disconnection |
| C5 | Ordered validation pipeline, per-reason counters, CRC-16 with Data ID | Black-channel coverage; never silently accept bad data |
| C6 | Parameters 10 ms, heartbeat 100 ms, 30 % ceiling, no staggering | Derived from bit rates; ≈10.5 % load |
| D1 | Static/live split, own parameters in same database, copy-out reads | One source of truth; safe access |
| D2 | Validity and freshness independent; 30 ms threshold; owner source status; 5 ms task; arrival-gap statistics | Covers transport and source staleness |
| D3 | Pattern by type, per-parameter sample periods; run-time fault injection deferred | Realistic multi-rate sensors |
| D4 | Same 5-slot layout on all nodes; full-domain ranges for now; INVALID keeps the old value but takes the new arrival time; `value_rejected` owned by `params/` | Every type and period on every node; an arriving frame is never shown as silence |
| D5 | Settle rule at the receiver for old Tx frames after a reconnect | Old values are never shown as current; works the same in Renode and on hardware |
| E1 | 16-byte rich heartbeat every 100 ms, the first at boot | Enables per-node recovery status; a restart is seen before its data |
| E2 | Seven link states on every node; 150 ms LATE, 350 ms OFFLINE | Early warning without false alarms |
| E3 | Restart = heartbeat passing checks 1–6 with sequence backwards and (BOOT or uptime down); uptime-wrap rule with 1 s slack; restart counter | Detects even very fast reboots; a corrupted frame cannot fake one |
| E4 | Periodic broadcast as resync; own state BOOT → RESYNCING → OPERATIONAL once every connected peer has sent an accepted block; never OPERATIONAL alone | Fast resync without waiting for dead nodes; no extra protocol |
| E5 | No failover; documented and demonstrated | Not required; parameter exchange has no single point of failure |
| E6 | NO_HEARTBEAT and DATA_INVALID states; owner-offline flag; recovery confirmed only by a later heartbeat | A live node with one failing path is not treated as dead; old queued heartbeats cannot confirm a recovery |
| E7 | `diag/` detects and reports only | No fault action can itself take a healthy node off the bus |
| F1 | 230,400 baud 8N1, no parity, no flow control; CRC-16 + COBS | ≈21 % load; unambiguous framing |
| F2 | SNAPSHOT 10 Hz + EVENT + CONFIG | Self-contained state plus no missed transitions |
| F3 | Payload in Protobuf (nanopb on MCU1), one schema for C and Python | The schema is the single specification; typed values; forward compatible; size known at compile time. CAN keeps fixed hand-written frames: both ends are this firmware and determinism matters more there |
| F4 | Transmit policy: latest wins for SNAPSHOT and CONFIG, 16-entry event queue, every frame numbered | Never blocks; never a backlog of old data; every gap explained |
| F5 | Events detected on change counters, and before every snapshot | No state is missed, however short; events explain the snapshot exactly |
| F6 | Network health = the weaker of online/4 and synchronised/20, computed on MCU1; LATE counts as online | One honest figure; a late heartbeat with data still arriving does not lower it |
| F7 | Host validation, CONNECTED/LOST/DISCONNECTED states, auto-reconnect, uptime total kept on the host | Old data never shown as live |
| F8 | Dashboard: local web page from the Python standard library, three sections, colour always with word and shape | No extra dependencies; readable without relying on colour |
| G1 | Thin `node.repl`, single `system.resc`, one Renode process, socket and file UART routing, 100 µs quantum | Reproducible setup; logs as deliverable |
| G2 | Scenarios S1–S4, self-checking | Every run gives the same events at the same times |
| G3 | Unit tests on the PC, mutation tests for every validation check, overload and counter-wrap tests | Each rule is shown to fail when broken |

# 15. Open items from earlier versions

Versions 0.2 and 0.3 listed six open items. Their numbers are kept because the repository documents refer to them.

| # | Item | Outcome |
|---|---|---|
| 1 | Restart handling against the sequence check | Closed: restart rule, 7.3 and decision E3 |
| 2 | Old Tx frames after a reconnect | Closed: settle rule, 6.2 and decision D5; for heartbeats, recovery confirmed by a later heartbeat, 7.2 |
| 3 | Command or event messages on CAN need a queue that never overwrites | Future scope (section 12); no such message exists |
| 4 | Queue sizing evidence | Closed: the longest main-loop pass is measured on every node (4.1) and is far below the 80 ms the queues cover; figures in `docs/sched_design.md` |
| 5 | Babbling-node health event | Future scope (section 12); the flooding node is already contained and counted |
| 6 | Check 8 per value; Q2; Q3 | Check 8 closed (5.4, 6.2). Q2 and Q3 remain questions for the hiring manager (section 13) |

# 16. Revision history

**Version 1.0: final (16 November 2026)**

| Section | Change |
|---|---|
| 1 | Final status; map of repository documents |
| 2.1 | Specification sheet added |
| 3 | Repository layout as built: `sched/`, `util/`, `proto/`, `dashboard/`, nanopb; uv for Python |
| 4.1 | Task table; timing rule; timing measurement |
| 4.2 | System clocks stated |
| 4.3 | Queue rules restated without build details; CAN transmit behaviour; UART buffers; memory placement |
| 5.4, 5.7 | Full-domain ranges; counters as an interface; restart heartbeat exception in check 7 |
| 6.2, 6.3 | INVALID + FRESH; settle rule; common 5-slot layout; own-parameter status |
| 7.2 | Link states as a table; DATA_INVALID added; OFFLINE → NO_HEARTBEAT; restart → RECOVERING |
| 7.3 | Restart rule decided; first heartbeat at boot; uptime-wrap rule |
| 7.4 | Own state as a table; DATA_INVALID peers not waited for |
| 8.1–8.5 | Protobuf payload; new sizes and link load; latest-wins transmit policy; event detection; network health; host accounting and uptime |
| 8.6 | Debug baud rate; dropped-text counter |
| 8.7 | Dashboard decided |
| 9 | Simulation constraints; final scenarios; verification required |
| 10, 11, 12, 14 | Updated to match |
| 15 | Open items closed or moved to future scope |

**Version 0.3: diag/ decisions (11 November 2026)**

| Section | Change |
|---|---|
| 6.1, 6.2 | Owner-offline flag per parameter |
| 7 | `diag/` detects and reports only; no fault actions |
| 7.2 | NO_HEARTBEAT state; UNKNOWN → OFFLINE after 350 ms; RECOVERING → ONLINE only on a heartbeat reporting OPERATIONAL; missed-count definition |
| 7.3 | Restart counted from uptime when an outage hid the restart flag |
| 7.4 | New own-state rule; resync timing |
| 7.5 | Owner-offline flag for P1–P5 on master failure |
| 8.6 | Rate-limited logging of rejected frames by reason |

**Version 0.2: per-sender receive queues (11 November 2026)**

| Section | Change |
|---|---|
| 3 | Host tests moved to `firmware/tests/host/`; `frame_queue` added to `drivers/` |
| 4.1, 4.3 | One CAN receive ring replaced by 4 per-sender queues × 8 slots that keep the newest frames, with lap detection; hardware Rx FIFO 0 in blocking mode |
| 5.7 | Check 0 also routes each accepted frame to its queue |
| 9, 10, 11, 14, 15 | Overload scenarios, queue cover row, rejected receive-path options, decision B3, open items |

**Version 0.1: design baseline (6 November 2026)**
