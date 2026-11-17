# FDCAN driver design

Covers `firmware/common/drivers/fdcan.c`. `RM` is RM0433 Rev 9, and `DS` is DS12110 Rev 11.

## Clock and pins

| Item | Value | Reference |
|---|---|---|
| Kernel clock | PLL2 Q, 80 MHz (`FDCANSEL = 10`) | RM p.413 |
| Maximum kernel clock at VOS0 | 125 MHz | RM Table 60, p.355 |
| Bus clock | APB1, `FDCANEN` in `RCC_APB1HENR` | |
| Clock calibration unit | Bypassed, `CDIV` = divide by 1, so the tq clock = 80 MHz kernel clock | RM p.2484, p.2488 |
| Time quanta clock ≤ APB clock | 80 MHz ≤ 120 MHz | RM p.2485 |
| RX, TX | PD0, PD1, AF9 | DS p.92 |

After reset the calibration unit gives no valid clock to the FDCAN (RM p.2484). `fdcan_init()` sets `FDCAN_CCU_CCFG.BCC` = 1 and `CDIV` = 0 while FDCAN1 has `INIT` and `CCE` set, which is when `CCFG` is writable. RM p.2582 calls bypass the default, but the reset value on p.2581 (`0x00000004`) has `BCC` = 0, so the driver sets it explicitly. Renode has only a placeholder at `0x4000A800`, so the write does nothing there.

## Bit timing

Both phases use a prescaler of 2, so 80 MHz / 2 = 40 MHz and one time quantum (tq) is 25 ns. The same tq in both phases follows CiA guidance.

Bit time in tq = 1 (sync) + TSEG1 + TSEG2. Sample point = (1 + TSEG1) / total. Registers hold each value minus 1 (RM p.2521, p.2526).

| Phase | Bitrate | Total tq | TSEG1 | TSEG2 | SJW | Sample point | Register |
|---|---|---|---|---|---|---|---|
| Nominal | 500 kbit/s | 40 MHz / 500 kHz = 80 | 63 | 16 | 15 | 64 / 80 = 80% | `NBTP = 0x1C013E0F` |
| Data | 2 Mbit/s | 40 MHz / 2 MHz = 20 | 14 | 5 | 5 | 15 / 20 = 75% | `DBTP = 0x00810D44` |

`_Static_assert` checks in `fdcan.c` stop the build if the bitrates no longer follow from the clock.

Nominal SJW is 15, one less than TSEG2, because RM p.2526 says `NSJW` must be smaller than `NTSEG2`. The data phase has no such rule (RM p.2521), so data SJW equals TSEG2, the CAN standard's limit. A `_Static_assert` checks the nominal rule.

## Transmitter delay compensation

At 2 Mbit/s a bit lasts 500 ns, so the transceiver loop delay is a large part of a bit. TDC compares the transmitted bit at a secondary sample point: measured delay + `TDCO` (RM p.2464–2465).

`TDCO` is counted in kernel clock periods (12.5 ns). Placing the secondary sample point at the 75% sample point gives (1 + 14) × 2 = 30, so `TDCR = 0x1E00`. `TDCF` stays 0.

Limits (RM p.2465): delay + `TDCO` must be under 6 data bits and at most 127. With `TDCO = 30`, this allows a loop delay up to 97 periods (about 1.2 µs). This value needs checking against the real transceiver.

## Message RAM

Base `0x4000AC00`, 10 KB, shared with FDCAN2 (unused). Access is 32-bit only. `fdcan_init()` clears the used area, because message RAM survives a reset.

Each element is 8 header bytes + 64 data bytes = 72 bytes, so any CAN FD frame fits and the validation pipeline can see its true length.

| Offset | Section | Size | Bytes |
|---|---|---|---|
| `0x000` | Standard filters | 8 × 4 | 32 |
| `0x020` | Rx FIFO 0 | 16 × 72 | 1152 |
| `0x4A0` | Tx FIFO | 4 × 72 | 288 |
| `0x5C0` | End | | 1472 of 10240 |

Rx FIFO 1, Rx buffers, Tx event FIFO and extended filters are not used.

## Filters

The caller passes ID ranges, so the driver stays unaware of the protocol. Each range becomes a range filter storing to Rx FIFO 0 (RM p.2516), and also names the RAM queue its frames go to (see below). Example word for `0x021`–`0x024`: `(1 << 27) | (0x021 << 16) | 0x024 = 0x08210024`.

`GFC = 0x2B` rejects non-matching frames and remote frames (RM p.2539).

## Receive path

1. A frame lands in Rx FIFO 0 and raises `RF0N` on interrupt line 0.
2. `FDCAN1_IT0_IRQHandler()` clears the flags, then moves each element into a RAM queue until the FIFO is empty. Priority 1, above SysTick and USART.
3. `fdcan_receive(queue, &frame)` copies the oldest frame of that queue out in the main loop.

**Hardware FIFO: blocking mode** (`RXF0C` = `0x00100020`). A full FIFO keeps its frames and drops the new one, setting `RF0L` (RM p.2476). The ISR has the highest priority and copies a frame in about a microsecond, so the FIFO fills only if the ISR cannot run at all (interrupts off, CPU halted, fault). Overwrite mode was tried (10 Nov 2026) and dropped: it needs a skip rule while full (RM p.2476) for a case that does not happen in normal running.

**RAM queues** (`drivers/frame_queue.c`): `FDCAN_RX_QUEUES` = 4 queues of 8 slots (76 bytes each, 2.4 KB in all, the same as the old 32-slot ring). The caller's range list is also the routing table: the ISR decodes the ID and uses the first matching range, like the hardware (RM p.2471). comm uses queue 0 for heartbeats and for its own IDs (only a duplicate node sends those), and one queue per peer for parameter blocks. A flood on one queue cannot push frames out of another.

**Each queue keeps its newest frames, without masking interrupts (lap detection).**
- `head` counts frames written and is written only by the ISR; `tail` counts frames taken and is written only by the main loop. Both count up forever; slot = index & 7.
- The ISR never checks for full: it writes slot `head & 7`, `__DMB()`, `head + 1`. When the queue is full, that slot holds the oldest frame, so the newest frame wins.
- `frame_queue_pop()`: if `head − tail > 8`, the ISR has lapped the reader; it counts `head − tail − 8` frames as overwritten and jumps to `tail = head − 8`, the oldest frame still intact. It copies that slot, then reads `head` again. Frame t is overwritten only when the ISR writes frame t + 8, which takes head past t + 8, and the ISR always finishes before the main loop continues. So `head − t ≤ 8` after the copy proves the copy is whole; otherwise it is thrown away and the loop goes round again.
- A retry needs a frame to arrive during a ~1 µs copy; frames are ≥ ~100 µs apart, so there is at most one retry in practice.
- Tested on the PC with forced timing (`firmware/tests/host/run_host_tests.sh`): lap, frame arriving during the copy (full and not full), 12 frames during one copy, counters wrapping past 2³². Removing either rule makes the test fail.

Counters: `rx_fifo_lost` (hardware `RF0L` events), `rx_overwritten[queue]` (counted by the reader when it notices a lap), `rx_unrouted` (cannot happen while filters and routes are the same list).

Measured with `renode/tests/stage4_overload.sh` (MCU3, node 1's frames after the overload):

| Overload on MCU3 | Before (one 32-slot ring, drop newest) | Now (4 queues, keep newest) |
|---|---|---|
| Main loop busy 1.5–2.0 s | seq 150–159, aged 500 → 410 ms | seq 192–200, aged 80 → 0 ms. Overwritten per queue: heartbeats 7, n1 42, n2 43, n4 43 = `lost` 135 |
| Same, and node 2 floods 20 frames/ms from 1.6 to 1.9 s | | Node 2's queue gave up 6043; the other queues exactly as above |
| CPU halted 1.5–2.0 s | Oldest 16 kept (hardware FIFO, blocking) | Same, by choice: `fifo_lost` = 1, `lost` = 149 |

## Transmit path

`fdcan_send()` checks `TFQF`, writes the element at the put index, then sets the matching `TXBAR` bit (RM p.2480). It never waits.

**Reconnect recovery.** In Renode, frames stay pending after a disconnect and only a new add request restarts them. When the FIFO is full, `fdcan_send()` writes `TXBAR = TXBRP`. Real hardware ignores requests for pending buffers (RM p.2553, note), so this is harmless there. `TXBCR` is not used, because it left the Renode FIFO stuck.

**Full Tx FIFO: refuse the new frame, do not overwrite.** The 4-frame FIFO fills only when nothing can leave: the node is off the bus, or no other node acknowledges. `fdcan_send()` then returns false and counts `tx_fifo_full`; comm counts `not_queued` and does not use a sequence number. Reasons:

- **Overwriting is not available.** A pending Tx frame cannot be rewritten in place; it must first be cancelled through `TXBCR`. In Renode the cancel left the FIFO stuck (`docs/setup_findings.md`), and cancel on dedicated Tx buffers was not tested.
- **It would gain nothing while the FIFO is full.** No receiver gets any frame then, old or new. The next periodic send, 10 ms later, offers the newest values again.
- **The cost is handled at the receiver.** After a reconnect the 4 waiting frames go out first, with old values. The settle rule holds the first block after a gap (`docs/params_design.md`), and the first heartbeat after a silence only makes a node RECOVERING (`docs/diag_design.md`). Both work the same in Renode and on hardware.
- **The sequence counts frames really queued.** A gap at the receiver then always means a frame lost on the bus. A refused frame that used up a number would be counted there as lost although it was never sent; refusals are counted at the sender instead (`not_queued`, `tx_fifo_full`).

Cancelling old frames at the sender is future scope (`docs/comm_design.md`), to be tried on the board.

## Renode limits

- `NBTP`, `DBTP` and `TDCR` writes are dropped, so they read back as reset values. The writes can be seen with `LogPeripheralAccess`.
- `FDCANSEL` is ignored.
- Frames are instant, with no bit timing, arbitration or error handling.

So the timing values above are checked by calculation only.
