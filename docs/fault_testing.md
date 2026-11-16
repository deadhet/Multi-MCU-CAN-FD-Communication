# Fault and break testing

Beyond the unit tests and the four assignment scenarios, the system is deliberately **broken** in Renode to prove it holds: corrupt frames, a flooded bus, a hung or reset node, a duplicate node, and 32-bit counters driven past their wrap. Each test targets a real-world failure, injects it in a reproducible way, and checks the nodes' logs against the expected, safe reaction.

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

The real firmware, with Renode injecting the fault. `run_scenarios.sh` plays S1–S4 and checks 49 expected log events in order: **49 passed, 0 failed** (204 s). The focused benches:

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

**One observation (not a fault).** Because the test forces the clock near its maximum *at boot*, each node briefly reports its peers OFFLINE for the first few milliseconds: a peer's timer still reads 0 while the clock reads ~49.7 days, so the first "age" looks huge until heartbeats arrive. A real node boots at ms = 0, so this never happens in practice; it is an artifact of starting the clock high at boot, and the test's verdict therefore measures the behaviour **across the wrap**, after the nodes have synced. See `study_material/test_report.md` for the note and a suggested robustness improvement.

Run time: about 1.5 min (build ~30 s, 12 s simulated ≈ 40 s, capture). The result is recorded here so the test need not run during a presentation.

## Running everything

```
renode/run_scenarios.sh                 # S1–S4, 49 checks
renode/check_uart_link.sh s1|s2|s3|s4   # the UART link end to end
renode/tests/stage4_mutate.sh <name>    # one corrupt-frame mutation
renode/tests/stage4_overload.sh <case>  # one receive-overload case
renode/tests/counter_overflow.sh        # the 32-bit counter wraps
```

Every break test leaves the committed source unchanged.
