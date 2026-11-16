# Multi-MCU CAN FD Communication and Monitoring

Four STM32H743VIT6 controllers share one CAN FD bus. Each owns five parameters, sends them every 10 ms, and keeps a validated copy of all 20. Every controller watches the others' heartbeats, detects when one stops, restarts or comes back, and re-synchronises its data. MCU1, the master, also sends its database and the network's health over UART to a Python web dashboard.

The whole system runs in Renode, four firmware images on one simulated bus. The firmware is bare-metal C (no RTOS, no dynamic memory) and is written for the real chip.

## Features

- **CAN FD** at 500 kbit/s arbitration and 2 Mbit/s data, with bit rate switching. Every frame carries a CRC-16 that includes its CAN ID, a version, the sender and a sequence number.
- **One owner per parameter.** Each node keeps all 20 parameters with value, source, sequence, age, validity and freshness. A stale or invalid value is never presented as current.
- **Health monitoring on every node:** heartbeats every 100 ms. Each peer's link is tracked as ONLINE, LATE, OFFLINE, RECOVERING, NO_HEARTBEAT or DATA_INVALID, with missed-heartbeat counts, restart detection and confirmed recovery.
- **Recovery and re-synchronisation:** a restarted or reconnected node is accepted again only once its heartbeats and data check out, and old frames still queued from before an outage are not taken as current.
- **UART link from MCU1:** Protobuf messages (one schema for C and Python) in COBS frames with a sequence number and CRC-16. A snapshot every 100 ms, an event on every change, and a configuration message every second.
- **Web dashboard:** network health, each MCU's health and all 20 parameters, updated live. On link loss the last data stays on screen, marked as old; values are never substituted.
- **Fault injection in Renode:** scripted scenarios, plus one terminal per MCU to pull a CAN cable, hang a CPU or reset a node by hand.

## How it works

```
          CAN FD bus (Renode CAN hub): 500 kbit/s arbitration, 2 Mbit/s data, BRS on
   ┌───────────────┬───────────────┬───────────────┬───────────────┐
 MCU1 (master)   MCU2            MCU3            MCU4
 owns P1–P5      owns P6–P10     owns P11–P15    owns P16–P20
   │
   │ USART3, 230,400 baud
   ▼
 Python dashboard → browser, http://127.0.0.1:8765
```

All four are equal nodes on the bus. MCU1 is the master because it monitors and reports, not because it relays: the others talk to each other directly.

| Item | Value | Details |
|---|---|---|
| CAN IDs | `(type << 4) \| node`: parameter blocks 0x021–0x024, heartbeats 0x031–0x034. The faster message wins arbitration; the low nibble is the sender | `docs/comm_design.md` |
| Parameter block | Every 10 ms, 32 bytes: 5 values, uint32 sequence, CRC-16 | `docs/comm_design.md` |
| Heartbeat | Every 100 ms, 16 bytes: state, synced count, sequence, uptime, error summary | `docs/comm_design.md` |
| Parameter STALE | 30 ms without an accepted block (3 periods) | `docs/params_design.md` |
| Node LATE / OFFLINE | 150 ms / 350 ms without a heartbeat | `docs/diag_design.md` |
| Recovery | The first heartbeat after OFFLINE gives RECOVERING; a later OPERATIONAL one confirms ONLINE | `docs/diag_design.md` |
| CAN bus load | ≈10.5 % worst case (calculated; Renode does not simulate bus timing) | |
| UART link | 230,400 8N1; ≈21 % load | `docs/uart_protocol.md` |
| Network health | The weaker of controllers online (ONLINE or LATE) out of 4 and parameters VALID and FRESH out of 20 | `docs/uart_protocol.md` |

## Getting started

**Tools** (Linux; these versions were used):

| Tool | Version | For |
|---|---|---|
| Ubuntu | 22.04.5 LTS, x86_64 | |
| Arm GNU Toolchain | 15.2.Rel1 (`arm-none-eabi-gcc` 15.2.1), on `PATH` | Firmware |
| CMake | 3.21 or newer (3.22.1 used) | Firmware build |
| gcc | 11.4 | Unit tests on the PC |
| Renode | 1.17.0, portable package for Linux, `renode` on `PATH` | Simulation |
| uv | 0.13.6 | Python 3.12 and the dashboard's packages (`protobuf`, `pyserial`), pinned in `dashboard/uv.lock` |

**Build and test** (from the repo root):

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build                          # build/node1.elf … node4.elf
firmware/tests/host/run_host_tests.sh        # C unit tests on the PC
(cd dashboard && uv sync --frozen && .venv/bin/python -m pytest -q)
```

## Running in Renode

```
renode/demo.sh                 # the dashboard plus a scripted run with faults (≈2.5 min)
renode/demo.sh s3              # the dashboard with one scenario: s1, s2, s3 or s4
renode/run_scenarios.sh        # S1–S4, with 49 checks on the nodes' logs
renode/check_uart_link.sh s1   # the UART link end to end, frame by frame; also s2, s3, s4
```

| Scenario | What happens | What every other node must report |
|---|---|---|
| S1 `s1_normal.resc` | Normal operation, 10 s | All OPERATIONAL, all ONLINE, 20/20 synchronised, every copy equal to its owner's value |
| S2 `s2_slave_failure.resc` | MCU3's CPU halts at 5 s and is reset at 10 s | MCU3 LATE → OFFLINE → P11–P15 flagged → RESTART → RECOVERING → ONLINE, while the rest keep exchanging |
| S3 `s3_disconnect.resc` | MCU3 is off the bus from 5 s to 15 s, no reset | The same without RESTART; MCU3, alone, goes RESYNCING until it is back |
| S4 `s4_master_failure.resc` | MCU1 halts at 5 s and is reset at 10 s | The slaves keep exchanging and recover MCU1; the dashboard shows the link LOST, then MCU1's restart |

Renode runs at about 0.3× real time here, so 10 simulated seconds take about 38 s. Each node's debug log goes to `logs/mcu1.log` … `logs/mcu4.log`: boots, state changes and a report every second.

**By hand:** one terminal per MCU, with commands such as `unplug 10`, `halt 5`, `reset` and step files:

```
renode/network.sh              # terminal 0: Renode with the four MCUs (paused) and the dashboard
renode/mcu.sh 1                # terminals 1–4, one per MCU; type "start" in any of them
```

Or in one terminal, typing Renode's own commands (`renode/prompt.sh`: Renode at its prompt plus the dashboard). All commands for both are in `docs/running_in_renode.md`.

## Dashboard

A header and three sections:
1. **Network:** controllers online, parameters synchronised, the health figure with its formula, and the CAN FD bit rates and BRS that MCU1 reports.
2. **MCUs:** status, heartbeat, last heartbeat age, missed heartbeats, communication timeout, and recovery and re-sync state, with an event log of every change.
3. **Parameters P1–P20:** value, source MCU, freshness and validity. STALE, INVALID and "owner offline" stand out.

It shows only what MCU1 sends. Against Renode it starts with `renode/demo.sh` or `renode/network.sh`. Against a board, connect MCU1's USART3 (TX, PD8) to a USB-UART adapter:

```
cd dashboard && .venv/bin/python -m noccarc_dashboard.server --serial /dev/ttyUSB0
```

The page design is described in `docs/dashboard_design.md`.

## Firmware architecture

```
 nodes/master/app_master.c, nodes/slave/app_slave.c   the app: wires the services, owns the task table
   │
 comm/      frames, validation (CRC with ID, length, version, source, sequence), restart rule
 params/    the 20-parameter database: value, source, sequence, age, validity, freshness
 diag/      every peer's link state, missed heartbeats, restarts, recoveries; this node's own state
 monitor/   UART link protocol (MCU1 only): Protobuf (nanopb), COBS, CRC, events, health
 sched/     cooperative task table on a 1 ms tick
   │
 drivers/   FDCAN, receive queues, transmit slots, UARTs, SysTick, clocks, cycle counter
```

- **Static memory only.** Every buffer and queue is sized at compile time, and nanopb runs with fixed limits.
- **Interrupt-driven reception.** The FDCAN interrupt, the highest priority, moves each frame from the hardware FIFO into RAM queues and stamps its arrival time. There is one queue for heartbeats and one per peer for parameter blocks. The main loop drains and validates them.
- **No interrupt masking** for shared data. Each queue has one writer per slot index and detects laps, and the UART transmit hand-over is one atomic exchange.
- **Time-triggered, non-blocking tasks** on a 1 ms SysTick:
  - parameters every 10 ms;
  - freshness and diag every 5 ms;
  - heartbeat every 100 ms;
  - debug report every 1 s;
  - on MCU1, a snapshot every 100 ms and a configuration message every 1 s.

  No task waits on hardware.
- **Services do not call each other.** The app copies data between them, so each module is unit-tested on the PC on its own.
- **One source tree, four images.** `NODE_ID` is set at compile time; MCU1 also links `monitor/`.

## Testing

| Level | What | Where |
|---|---|---|
| Unit, on the PC | 6 C suites: receive queues, params, diag, scheduler, transmit slots, UART protocol | `firmware/tests/host/` |
| Unit, Python | Frame decoding (against frames made by the C code), link states, uptime wrap, events, page model, web server | `dashboard/tests/` |
| System, Renode | Scenarios S1–S4: 49 checks that the expected events appear, in order, in the nodes' logs | `renode/run_scenarios.sh` |
| UART link, Renode | Every frame decoded while a scenario runs and compared with MCU1's own log | `renode/check_uart_link.sh` |
| Fault benches, Renode | Halt, reset, reconnect, a duplicate node ID, overload, and mutation tests: one node's frames deliberately broken (bad CRC, bad values, a fake restart), each of which the receivers must reject or flag | `renode/tests/` |

## Documentation

| Document | Covers |
|---|---|
| `docs/architecture_flow.md` | End-to-end code flow: the `main()` super-loop, the task table, and the CAN/UART paths down to the drivers and ISRs |
| `docs/comm_design.md` | Frame layouts, validation order and counters, sequence and restart handling |
| `docs/fdcan_design.md` | FDCAN driver: clocks, bit timing, filters, receive and transmit paths |
| `docs/params_design.md` | The parameter table and database rules, freshness, validity, the settle rule after reconnects |
| `docs/diag_design.md` | Link and own states, timeouts, restart and recovery rules |
| `docs/sched_design.md` | The scheduler and its timing measurements |
| `docs/uart_protocol.md` | The UART link: frame, messages, fields, a worked hex example, error and loss handling |
| `docs/dashboard_design.md` | The dashboard: data sources, display rules, safety |
| `docs/fault_testing.md` | How the system is deliberately broken in Renode (corrupt frames, overload, node failures, counter wraps) and how it holds |
| `docs/running_in_renode.md` | Every script and command for running and driving the simulation |
| `docs/setup_findings.md` | Tool versions, and what Renode 1.17's STM32H743 and CAN models do and do not support |

The design documents also record the measured results and the options considered and rejected.

## Repository layout

```
firmware/
  common/          drivers, comm, params, diag, monitor, sched, util; system_table.c (the 20 parameters)
  nodes/           app_master.c (MCU1), app_slave.c (MCU2–MCU4)
  startup/, linker/, third_party/   start-up code, linker script, CMSIS, nanopb
  tests/host/      C unit tests run on the PC
proto/             monitor.proto: the UART payload, single source for C and Python
dashboard/         Python web dashboard (uv project) and its tests
renode/            platform and system scripts, scenarios, demo, MCU terminals, step files
  tests/           fault benches
scripts/           gen_proto.sh: regenerates the Protobuf code
docs/              design documents
cmake/             arm-none-eabi toolchain file
```

## Limitations

- **The simulated bus is logical.** Renode's CAN hub delivers whole frames, with no bit timing, arbitration timing, ACK slot, error frames or bus-off. The firmware configures 500 kbit/s, 2 Mbit/s and BRS, but the simulation cannot measure them.
- **Renode does not keep writes to the FDCAN bit-timing registers,** so the bit rates the dashboard shows come from the constants that program them.
- **Renode does not pace the UART:** bytes leave at once, so the busy-link behaviour is tested only on the PC.
- **Timing:** Renode runs at about 0.3× real time and simulates the CPU at a fixed 100 MIPS. Measured run times are Renode's, not the chip's.
- **One node at a time:** pausing Renode pauses every node, so a single failure is made by halting one CPU or disconnecting one node from the CAN hub.

## Known issues

- **Renode 1.17 crashes** if a node is disconnected from the CAN hub twice in a row. The scripts always reconnect in between; by hand, plug the cable back first.
- **Port 3456**, Renode's UART terminal for the dashboard, listens on every network interface. It carries read-only data, but anyone on the same network could read it: use a firewall on untrusted networks.
- **No watchdog yet:** a node whose main loop hangs is detected by the others but does not reset itself.
