# Scheduler: design and measurements

Covers `firmware/common/sched/` and `firmware/common/drivers/cycles.c`. Every node runs it.

## How tasks run

A cooperative, time-triggered task table (a "super-loop"). Every pass of `main()`:
1. `sched_pass_mark()`: one mark per pass, for the longest-pass measurement.
2. `sched_run_due()`: walks the table in order. For each task it reads the ms clock, and if `now - last_run >= period` it sets `last_run = now` and runs the task to the end.
3. The app drains the CAN receive queues.

Tasks never interrupt each other; only interrupts do (FDCAN, then SysTick, then the UARTs). So every task must be short and must never wait. Table order decides who goes first when several are due in the same pass. The report runs before the snapshot on MCU1, so both show the same values.

| Node | Tasks (period, first run) |
|---|---|
| All | `param` 10 ms (10), `fresh` 5 ms (5), `diag` 5 ms (5), `hb` 100 ms (0), `report` 1000 ms (5) |
| MCU1 | also `snap` 100 ms (5), `cfg` 1000 ms (0) |

## Measurements (Het, 12 Nov 2026: measure first)

Worst cases since boot, printed in the 1 s report:

    MCU1 sched pass=1109us param=29us/0ms/0 fresh=62us/1ms/2 diag=31us/1ms/2 hb=18us/0ms/0 report=255us/0ms/0 snap=877us/0ms/0 cfg=457us/0ms/0

- `pass`: the longest main-loop pass: the tasks that ran, the CAN drain, and the interrupts in between.
- `name=run/late/count`:
  - `run`: the task's longest run, interrupts included, in µs, rounded up.
  - `late`: its latest start after it was due, in ms.
  - `count`: how many of its runs started 1 ms or more late.
- Source: the DWT cycle counter (`drivers/cycles.c`), 480 cycles per µs. It wraps every 8.9 s; only differences are used.
- **Renode times are not the chip's.** Renode runs a fixed 100 million instructions per second, with no flash wait states or caches. The numbers compare tasks with each other; real times need the board.
- The measurements change nothing about when tasks run. `test_sched.c` checks that: a copy switched to fixed-rate timing fails it.

First readings, Renode, S1–S4 (12 Nov 2026):

| Node | Longest pass | Late starts in 15 s |
|---|---|---|
| MCU1 | 1103–1146 µs (`snap` 868–886 µs, `cfg` 437–458 µs, `report` 236–252 µs) | `fresh` and `diag` 1 ms late twice; `report` 1 ms late once (S1–S3); `param` never |
| MCU2–MCU4 | 222–271 µs (`report` ≈ 220 µs) | none |

- MCU1's passes are just over 1 ms, Renode's snapshot encode being most of it. A task that falls due in such a pass starts 1 ms late.
- The report shows the drift described below. In S1 it ran at 1005, 2005, then 3006, 4006, … 9006. One late start shifted every later run.
- MCU1 still sent exactly 100 parameter blocks per second (`tx p=900` at 9 s).
- The scenario checks allow for that 1 ms.

## Considered, not taken (yet): fixed-rate timing

`due += period` instead of `last_run = now` would keep every task on a fixed grid. A late run would then not delay the next, and missed periods would be counted.

Its trade-offs:
- Gaps after a late run can be shorter than the period (the settle rule may hold one extra block).
- The same tasks always fall due together.
- Under overload, tasks late in the table starve.
- Like today, a long task still blocks the others.

Decision (Het, 12 Nov 2026): measure first, and switch only if the measurements show lateness that matters. So far:
- 1 ms late starts, on MCU1 only, caused by Renode's slow encode.
- A 1 ms shift of the 1 s report.
- No drift of the 10 ms sends.

## Worst cases of the current scheduler

1. A task never returns: only interrupts keep running, and peers mark the node OFFLINE at 350 ms. Without a watchdog (not decided), it stays silent.
2. A task runs long: everything behind it, and the CAN drain, waits. Peers settle after a 30 ms gap in its blocks, mark it LATE at 150 ms and OFFLINE at 350 ms. The RAM queues keep the newest 8 frames per sender.
3. A start 1 ms late or more shifts every later run of that task (`last_run = now`): the rate drifts down.
4. Missed runs are not caught up. The `late` figure shows it.
5. Pile-up: at t = 1005, 2005, … the most tasks fall due in one pass. That is the `pass` maximum.

## Tests

| Test | Shows |
|---|---|
| `firmware/tests/host/test_sched.c` (`SCHED PASS`) | Timing unchanged (due rule, late run moves the next, no catch-up); longest run, latest start, late count; cycle-counter wrap; longest pass; ms wrap; report line. Three copies with one rule broken fail: lateness 5, longest run 4, fixed-rate timing 3 |
| `renode/run_scenarios.sh` | S1–S4 unchanged with the measurements in place |
