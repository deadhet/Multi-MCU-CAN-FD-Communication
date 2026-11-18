# Fault and break testing

Beyond the unit tests and the four assignment scenarios, the system is deliberately **broken** in Renode to prove it holds: corrupt frames, a flooded bus, a hung or reset node, a duplicate node, and 32-bit counters driven past their wrap. Each test targets a real-world failure, injects it in a reproducible way, and checks the nodes' logs against the expected, safe reaction. Sections 5 and 6 list the scenario checks (`run_scenarios.sh`) and the unit tests on the PC (`run_host_tests.sh`).

All results below are from a full run on 16 Nov 2026 (Renode 1.17.0). Run times are wall-clock on the development PC (Renode runs at about 0.3× real time).

## How a break test works

Each test is a shell script in `renode/tests/`. They share one idea: change exactly one thing, build, run four nodes, and read the logs.

- **Copy-and-break** (`stage4_mutate.sh`, `stage4_overload.sh`): copy the source tree to `build/<kind>/<name>`, change one line there, build and run that copy. The committed source is never touched.
- **Scenario injection** (`*.resc`, `run_scenarios.sh`): the real firmware, with Renode halting a CPU or disconnecting a node from the CAN hub at set times.
- **Edit-and-restore** (`counter_overflow.sh`): edit three real files in place, build, run, then restore them. A trap restores the files even on error, so `git diff` is empty afterwards.

A node's debug log (`logs/mcuN.log`) carries a once-a-second report (`tx`, `rx`, `err`, `drv`, `db synced`, `diag self`) and a line for every state change, which is what the checks read.

## 1. Corrupt frames — `stage4_mutate.sh <name>`

**Real-world cause:** electrical noise, a firmware fault in one node, or a misconfigured peer putting malformed CAN frames on the bus. **Simulated:** node 2's transmit path is changed to emit one kind of bad frame; the other three nodes must reject it and count the reason, and never let a bad value reach the database (the "golden rule").

| Mutation | Real-world case | Every receiver shows |
|---|---|---|
| `crc` | A bit flips in flight | `crc=100`; P6–P10 never received, `synced=15` |
| `version` | A node runs an older/newer protocol | `ver=100`; rejected |
| `source` | A node mislabels its ID | `src=100`; rejected |
| `length` | A truncated or padded frame | `len=100`; rejected |
| `nobrs` | A node sends classic CAN, not CAN FD | `fmt=111` (MCU1 `110`: one boot-time frame); rejected |
| `dup` | A stuck sequence counter | `dup=99`; duplicates dropped |
| `old` | Sequence counting backwards | `old=99`; stale frames dropped |
| `gap` | Frames lost on the bus (seq +2) | `lost=99`, frames still **accepted**: a gap is not corruption |
| `badid` | A node transmits an out-of-range ID | MCU1 (extra filter) `id=100`; MCU3/MCU4 drop it in hardware, all counters 0 |
| `fakeboot` | A corrupted frame faking a restart | `crc=2`, `r=0`: a frame that fails the CRC can never trigger the restart rule |
| `values` | A sensor sends NaN / fault / out-of-range | `synced=15`: P6, P7, P9, P10 INVALID, P8 VALID-but-STALE; all CAN counters 0 (the frames are valid, the values are not) |
| `hbcrc` | Only the heartbeats corrupt, data fine | `NO_HEARTBEAT`, never OFFLINE: the node is known alive by its data |

In every case node 2's own database is unaffected and the other streams are untouched. ~13 s per mutation.

## 2. Receive overload — `stage4_overload.sh <stall|halt|babble>`

**Real-world cause:** a node's main loop is briefly busy, its CPU stalls, or one node floods the bus — any of which can overrun receive buffers. **Simulated:** MCU3 is instrumented to print what it drains after the overload; the queues must keep the **newest** frames and account for the rest.

| Case | Real-world case | Result |
|---|---|---|
| `stall` | MCU3's main loop busy 1.5–2.0 s; the CAN interrupt still runs, so the RAM queues overflow | MCU3 drains 34 at once, keeping node 1's newest (seq 192–199, aged 80→10 ms); `lost=135` = the sum the queues overwrote, no gap passed downstream |
| `halt` | MCU3's CPU halted 1.5–2.0 s; nothing runs, so the 16-deep hardware FIFO overflows | The FIFO keeps 16; MCU3 reports `GAP n1 seq=200 lost=46`, `fifo_lost=1`, `lost=149` |
| `babble` | `stall`, and node 2 floods 20 blocks/ms | MCU3's node-2 queue absorbs it (`ovw=7/42/6043/43`): only node 2's own stream loses frames; heartbeats, node 1 and node 4 are exactly as in `stall`. MCU1 (not stalled) processes node 2 normally |

The design point proven: one noisy or stalled path never starves the others, and ages stay correct because arrival time is stamped in the interrupt. ~build + run each.

## 3. Node failures — the scenario benches

The real firmware, with Renode injecting the fault. `run_scenarios.sh` plays S1–S4 and checks 49 expected log events in order: **49 passed, 0 failed** (204 s); every check is listed in section 5. The focused benches:

| Bench | Real-world case | Result |
|---|---|---|
| `stage4_halt` | A node's CPU hangs for 0.5 s | n3 LATE 1550, OFFLINE 1750, RECOVERING 2000, ONLINE 2100; P11–P15 STALE 1525, FRESH 2015 (`held=1`); **no restart** (uptime kept rising) |
| `stage4_reconnect` | A loose CAN cable, 1 s | OFFLINE 1750, RECOVERING 2500, ONLINE 2600, `held n3=3` (the frames queued in the Tx FIFO) |
| `stage4_reset` | A node reboots | RESTART + RECOVERING at 1499 (`r=1`), ONLINE 1600; never LATE (the gap was under the threshold) |
| `stage4_dup_node` | Two nodes with the same ID | Both copies and the others count `dupnode`/`dup`; the absent node 3 goes OFFLINE; no false restart |
| `stage6_old_hb_then_boot` | A node reconnects, old frames arrive, then it resets | An old queued OPERATIONAL heartbeat gives only RECOVERING; the boot heartbeat counts one restart; ONLINE only on a later OPERATIONAL heartbeat |
| `stage3_loopback` | The FDCAN driver in isolation | `LOOPBACK PASS` |

Cross-checks on the clean 4-node run: every receiver's accepted frame count equals the sender's (48/48 pairs), and all 180 parameter copies equal their owner's value (0 problems).

**Over the UART link** (`check_uart_link.sh s1`…`s4`): every frame decoded while the scenario runs, compared with MCU1's own log. All four pass with 0 missing and 0 lost frames; S4 shows exactly one MCU1 restart and one LOST period, as expected.

## 4. Counter overflow — `counter_overflow.sh`

**The testcase.** The firmware uses 32-bit counters that advance the whole time a node is powered. They wrap only after a very long uptime:

| Counter | Wraps after | Used by |
|---|---|---|
| Millisecond clock (`systick.c`) | 49.7 days | Scheduler due times, heartbeat uptime, every age and timeout |
| CAN parameter-block sequence (`comm.c`) | 497 days | Receiver sequence check (lost/duplicate/old), restart rule |
| CAN heartbeat sequence | 13.6 years | Sequence check and restart rule |
| UART frame sequence (`monitor.c`) | even later | Host gap detection and uptime unwrap |

**Targeted real-world scenario.** A controller left running for weeks or months — exactly what a deployed device does. When the millisecond clock passes 2³², a naive "time since last heartbeat = now − last" would briefly compute a huge value and wrongly declare every peer OFFLINE; a naive sequence check would read the wrap as a flood of lost frames or a restart. The whole design uses **unsigned subtraction** so a difference stays correct across the wrap, and the host decides wrap-versus-restart from the frame sequence. This test proves that empirically, in seconds instead of 49.7 days.

**How it is simulated.** A real wrap takes 49.7+ days, so the test starts each counter just below its maximum at boot and lets it wrap a few seconds into the run. It edits three source lines in place:

| File | Counter start | Wraps about |
|---|---|---|
| `systick.c` | `ms_counter = 0xFFFFF000` | 4.1 s |
| `comm.c` | `tx_seq = {0xFFFFFFCE, 0xFFFFFFCE}` | 0.5 s (param), 5 s (heartbeat) |
| `monitor.c` | `uart_seq = 0xFFFFFFE2` | 3 s |

It builds, runs four nodes for 12 s of simulated time while capturing MCU1's UART link, then restores the three files (a trap restores them even on failure; `git diff` is empty afterwards). The committed firmware keeps its real default of 0.

**Result: PASS.** The millisecond clock crossed 2³² during the run (observed: `t` jumped from 4294967201 to 905). Across that crossing:

- every node stayed OPERATIONAL with `synced=20`, `offline=0` — no OFFLINE, LATE, RECOVERING or RESTART once it had synced;
- the CAN sequence wraps were absorbed by the signed-difference check: no false `old`, `dup`, `lost` or restart;
- the host decoded all 149 UART frames with **0 missing, 0 lost, 0 false restarts**, and unwrapped MCU1's uptime to **49 d 17:02:55 (count wrapped 1 time)**; 119 snapshots replayed with 0 differences.

**One observation (not a fault).** Because the test forces the clock near its maximum *at boot*, each node briefly reports its peers OFFLINE for the first few milliseconds: a peer's timer still reads 0 while the clock reads ~49.7 days, so the first "age" looks huge until heartbeats arrive. A real node boots at ms = 0, so this never happens in practice; it is an artifact of starting the clock high at boot, and the test's verdict therefore measures the behaviour **across the wrap**, after the nodes have synced. A possible hardening, not applied: start each peer's last-heartbeat time at the clock's value in `diag_init()` instead of 0. At a real boot (ms = 0) this changes nothing, but the diagnostics would then be right for any boot clock value.

Run time: about 1.5 min (build ~30 s, 12 s simulated ≈ 40 s, capture). The result is recorded here so the test need not run during a presentation.

## 5. The scenario checks — `renode/run_scenarios.sh`

**What it does.** Runs the four assignment scenarios in Renode one after the other, keeps each node's log in `build/scenarios/<scenario>/mcuN.log`, and checks the logs: one PASS or FAIL line per check, then `## 49 passed, 0 failed`; it exits with 1 if any check fails. The checks look for log lines in a required order, count lines, read the last report and the report at 7005 ms (4 ms of slack for the scheduler's drift), and run two cross-checks: `renode/tests/stage4_crosscheck.awk` (every receiver accepted every frame each sender queued) and `renode/tests/stage5_params_crosscheck.awk` (every parameter copy equals its owner's value at the same report).

| Scenario | Fault injected | Checks | Each check requires |
|---|---|---|---|
| S1, `s1_normal.resc` | None, 10 s | 11 | Each of the 4 nodes: OPERATIONAL, `synced=20`, `never=0` at the end; no LATE, OFFLINE, RECOVERING, NO_HEARTBEAT or DATA_INVALID line ever. All nodes: every parameter copy equals its owner's value at every report; every frame queued was accepted (boot losses listed separately); no frame rejected and no debug text dropped |
| S2, `s2_slave_failure.resc` | MCU3 halted at 5 s, reset at 10 s, run to 15 s | 13 | MCU1, MCU2, MCU4 each: n3 LATE → OFFLINE → owner-offline flag on P11–P15 → RESTART → RECOVERING → flag cleared → ONLINE recovered, in this order; `synced=15` in the 7 s report (still exchanging, only P11–P15 stale); own state never left OPERATIONAL; at the end `synced=20` and n3 restarted once, recovered once. MCU3 after its reset: OPERATIONAL, `synced=20` |
| S3, `s3_disconnect.resc` | MCU3 off the bus 5–15 s, no restart, run to 20 s | 12 | MCU1, MCU2, MCU4 each: n3 LATE → OFFLINE → flag → RECOVERING → flag cleared → ONLINE recovered; no restart counted; at the end `synced=20`, n3 recovered once. MCU3: all peers OFFLINE → self RESYNCING → peers RECOVERING → self OPERATIONAL → peers ONLINE recovered; at the end OPERATIONAL, `synced=20`. All nodes: parameter copies equal their owner at every report outside the outage |
| S4, `s4_master_failure.resc` | MCU1 halted at 5 s, reset at 10 s, run to 15 s | 13 | MCU2, MCU3, MCU4 each: n1 LATE → OFFLINE → flag on P1–P5 → RESTART → RECOVERING → flag cleared → ONLINE recovered; `synced=15` in the 7 s report (the slaves keep exchanging without the master); own state never left OPERATIONAL; at the end `synced=20`, n1 restarted once, recovered once. MCU1 after its reset: OPERATIONAL, `synced=20` |

The checks were also run against the wrong logs, where they fail. The times of every event in these runs are in `docs/diag_design.md` (Results).

## 6. Unit tests on the PC — `firmware/tests/host/run_host_tests.sh`

**What it does.** The services and the transmit buffers are plain C without register access, so the script compiles each module with its test using the PC's `gcc` (`-Wall -Wextra -Wshadow -Werror`), runs it, and then runs the dashboard's Python tests (`uv run pytest`). Times are passed in as numbers, so every boundary can be driven at an exact millisecond, and moments that cannot be forced on the target, such as a frame arriving in the middle of a copy, are played step by step. Each C suite prints one PASS line; the script stops at the first failure.

| Suite, PASS line | Code under test | Covers | Details |
|---|---|---|---|
| `test_frame_queue.c`, `FRAME QUEUE PASS` | `drivers/frame_queue.c` | 8 cases: each receive queue keeps its newest frames, lap detection and the overwrite count, a frame arriving while the main loop copies one | `docs/comm_design.md`, `docs/fdcan_design.md` |
| `test_params.c`, `PARAMS PASS` | `params/`, `system_table.c` (as node 2) | The parameter table and init; the 30 ms stale boundary; the settle rule (reconnect replay, 30/31 ms and 4/5 ms boundaries); own and unknown senders; check 8; own source stale and fault; arrival gaps across the ms wrap; the simulated sensors; the owner-offline flag | `docs/params_design.md` |
| `test_diag.c`, `DIAG PASS` | `diag/` (as node 1) | Boot and never heard; the LATE and OFFLINE boundaries (149/150 ms, 349/350 ms); the first heartbeat after OFFLINE; restart by comm's flag and by uptime; the uptime wrap; NO_HEARTBEAT; DATA_INVALID entered at 350 ms and ended by a block; own state; the ms wrap; the report line | `docs/diag_design.md` |
| `test_sched.c`, `SCHED PASS` | `sched/` | The due rule, a late run moving the next, no catch-up; longest run, latest start, late count; cycle-counter and ms wraps; longest pass; the report line | `docs/sched_design.md` |
| `test_tx_slots.c`, `TX SLOTS PASS` | `drivers/tx_slots.c` | The writer (main loop) and the mover (UART interrupt) played step by step: fixed cases, the event queue, then 200,000 random steps checked against the rules after every step | `docs/monitor_design.md` |
| `test_monitor.c`, `MONITOR PASS` | `monitor/`, `cobs.c`, `crc16.c`, `tx_slots.c`, nanopb (as node 1) | COBS (known vectors, 254/255-byte groups, 500 random round trips); every snapshot field decoded back; CONFIG; latest wins on a busy link; the health table; events in order, grouping, a full queue; the worst-case snapshot size. Writes `build/host/monitor_frames.bin` for the Python test | `docs/uart_protocol.md` §11 |
| `dashboard/tests/`, `88 passed` | `dashboard/noccarc_dashboard/` | `test_framing.py` (14): COBS, CRC, frame checks, and decoding the C test's frames to the same values. `test_link.py` (10): link states and the 300 ms limit, garbage, frames in pieces, joining mid-frame, gaps replaced or lost, MCU1 restarts. `test_uptime.py` (8): uptime across the 49.7-day wrap. `test_events.py` (7): events replayed against snapshots and against MCU1's log. `test_model.py` (39): what the page shows in every state. `test_server.py` (10): the served files, security headers, host check | `docs/dashboard_design.md`, `docs/uart_protocol.md` §11 |

**Testing the tests.** Copies of the code with one rule broken must make the suites fail, and they do: in `params/`, four broken copies fail 7, 9, 2 and 2 checks; in `diag/`, seven broken copies fail 2 to 9 checks each; in `sched/`, three broken copies fail 5, 4 and 3 checks; and the dashboard tests catch three mutants (LATE shown as normal, no host check, any file served).

## Running everything

```
firmware/tests/host/run_host_tests.sh   # C unit tests and the dashboard's Python tests, on the PC
renode/run_scenarios.sh                 # S1–S4, 49 checks
renode/check_uart_link.sh s1|s2|s3|s4   # the UART link end to end
renode/tests/stage4_mutate.sh <name>    # one corrupt-frame mutation
renode/tests/stage4_overload.sh <case>  # one receive-overload case
renode/tests/counter_overflow.sh        # the 32-bit counter wraps
```

Every break test leaves the committed source unchanged.
