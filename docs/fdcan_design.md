# FDCAN driver design

Covers `firmware/common/drivers/fdcan.c`. `RM` is RM0433 Rev 9, and `DS` is DS12110 Rev 11.

## Clock and pins

| Item | Value | Reference |
|---|---|---|
| Kernel clock | PLL2 Q, 80 MHz (`FDCANSEL = 10`) | RM p.413 |
| Maximum kernel clock at VOS0 | 125 MHz | RM Table 60, p.355 |
| Bus clock | APB1, `FDCANEN` in `RCC_APB1HENR` | |
| RX, TX | PD0, PD1, AF9 | DS p.92 |

## Bit timing

Both phases use a prescaler of 2, so 80 MHz / 2 = 40 MHz and one time quantum (tq) is 25 ns. The same tq in both phases follows CiA guidance.

Bit time in tq = 1 (sync) + TSEG1 + TSEG2. Sample point = (1 + TSEG1) / total. Registers hold each value minus 1 (RM p.2521, p.2526).

| Phase | Bitrate | Total tq | TSEG1 | TSEG2 | SJW | Sample point | Register |
|---|---|---|---|---|---|---|---|
| Nominal | 500 kbit/s | 40 MHz / 500 kHz = 80 | 63 | 16 | 16 | 64 / 80 = 80% | `NBTP = 0x1E013E0F` |
| Data | 2 Mbit/s | 40 MHz / 2 MHz = 20 | 14 | 5 | 5 | 15 / 20 = 75% | `DBTP = 0x00810D44` |

`_Static_assert` checks in `fdcan.c` stop the build if the bitrates no longer follow from the clock.

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

The caller passes ID ranges, so the driver stays unaware of the protocol. Each range becomes a range filter storing to Rx FIFO 0 (RM p.2516). Example word for `0x021`–`0x024`: `(1 << 27) | (0x021 << 16) | 0x024 = 0x08210024`.

`GFC = 0x2B` rejects non-matching frames and remote frames (RM p.2539).

## Receive path

1. A frame lands in Rx FIFO 0 and raises `RF0N` on interrupt line 0.
2. `FDCAN1_IT0_IRQHandler()` clears the flag, then moves each element into the 32-slot ring. Priority 1, above SysTick and USART.
3. `fdcan_receive()` copies the oldest slot out in the main loop.

The ring has one producer and one consumer. Indices run freely, so all 32 slots are usable: used = head − tail. Slot data is written, then `__DMB()`, then head moves. A full ring drops the newest frame and counts it.

## Transmit path

`fdcan_send()` checks `TFQF`, writes the element at the put index, then sets the matching `TXBAR` bit (RM p.2480). It never waits.

**Reconnect recovery.** In Renode, frames stay pending after a disconnect and only a new add request restarts them. When the FIFO is full, `fdcan_send()` writes `TXBAR = TXBRP`. Real hardware ignores requests for pending buffers (RM p.2552), so this is harmless there. `TXBCR` is not used, because it left the Renode FIFO stuck.

## Renode limits

- `NBTP`, `DBTP` and `TDCR` writes are dropped, so they read back as reset values. The writes can be seen with `LogPeripheralAccess`.
- `FDCANSEL` is ignored.
- Frames are instant, with no bit timing, arbitration or error handling.

So the timing values above are checked by calculation only.
