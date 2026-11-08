# Stage 0 setup findings

All results below were measured on Renode 1.17.0 with the probe firmware in
`docs/stage0_probe/`, unless marked "from source". Each test ran twice with the same result.

## Tool versions

| Tool | Version | Location |
|---|---|---|
| Renode | 1.17.0 (1.17.0+20260907gitf1dd1b4af), portable | `~/tools/renode`, command in `~/.local/bin` |
| Arm GNU Toolchain | 15.2.Rel1 (Build arm-15.86), gcc 15.2.1 | `~/tools/arm-gnu-toolchain`, commands in `~/.local/bin` |
| CMake | 3.22.1 (already installed) | `/usr/bin/cmake` |
| Host | Ubuntu 22.04.5, x86_64 | |

Both downloads matched their published SHA-256. Both tools run from any folder, for this user, without sudo.
Arm 16.1 and 15.3 download URLs returned 404, so 15.2.rel1 is the newest found.

## Stock platform (`platforms/cpus/stm32h743.repl`, which uses `stm32h7.repl`)

| Item | Finding |
|---|---|
| FDCAN model | `CAN.MCAN`. `fdcan1` at `0x4000A000`, `fdcan2` at `0x4000A400` |
| FDCAN IRQs | Line0 → NVIC 19 (tested), Line1 → NVIC 21 (not tested) |
| Message RAM | `0x4000AC00`, size `0x2800` (10 KB), shared by both controllers |
| UART model | `UART.STM32F7_USART`. `usart1` `0x40011000`, `usart3` `0x40004800` |
| UART IRQ route | Through EXTI: USART1 line 26 → NVIC 37. Works with no EXTI setup |
| RCC | `STM32H7_RCC`, with SysTick linked to the system clock |
| PWR | Python stub returning fixed ready flags |

## Clock

- No ready-flag wait hangs: VOSRDY, PLL1RDY, PLL2RDY and SWS all passed. The HSI fallback is not needed.
- `sysbus.nvic Frequency` reads 400 MHz after the PLL switch and 64 MHz on HSI.
- The ms counter tracks virtual time: `t=3300` at 3.32 s of virtual time.
- PLL2 reports ready, but from source `FDCANSEL` is a tag, so kernel clock selection has no effect in the model.

## Working Monitor commands

| Purpose | Command |
|---|---|
| Create hub | `emulation CreateCANHub "canHub"` |
| Connect node | `connector Connect sysbus.fdcan1 canHub` |
| Disconnect node | `connector Disconnect sysbus.fdcan1 canHub` |
| Pick machine | `mach set "mcu3"` |
| Halt one machine | `sysbus.cpu IsHalted true` (resume with `false`) |
| Reset one machine | `machine Reset` (needs the reset macro below) |
| UART to TCP | `emulation CreateServerSocketTerminal 3456 "uartTerm" false`, then `connector Connect sysbus.usart3 uartTerm` |
| UART to file | `sysbus.usart1 CreateFileBackend @path true` |
| Trace registers | `sysbus LogPeripheralAccess sysbus.fdcan1 true` (INFO level) |
| Set quantum | `emulation SetGlobalQuantum "0.0001"` |
| Read quantum and time | `emulation GetTimeSourceInfo` |
| Timed run | `emulation RunFor "1"` |
| Headless run | `renode --disable-gui --console -e "include @file.resc" </dev/null` |

Commands that do not work as hoped:
- `machine Pause` on one machine does not hold: the next `RunFor` resumes every machine. Use `IsHalted`.
- `emulation GetGlobalQuantum` does not exist.
- A failed command stops the `.resc` and leaves Renode at the prompt. `</dev/null` makes it exit, and every run should still use `timeout`.
- In scripts, write `$ORIGIN/file` without `@`.

## Behaviour by scenario

**Normal, 4 nodes, 100 µs quantum.** Each node saw every other node at a 10 ms gap with no loss. `FDF` and `BRS` reach the receiver (`fd=267 brs=267`), so validation check 2 will work.

**Halt (S2, S4).** Peers stop seeing the node: gap 510 ms for a 500 ms halt. SysTick does not count during a halt, so the halted node's ms clock falls behind by the halt length. Its FDCAN keeps receiving, the 8-deep test FIFO overflowed, and frames were lost on resume (`lost=3`). Peers keep running.

**Reset (S2, S4).**
- `IsHalted` is cleared, so a halted node restarts after `machine Reset`.
- RAM survives reset: DTCM, AXI SRAM and message RAM all kept a test value. Firmware must initialise `.bss`, `.data` and message RAM itself.
- FDCAN registers do reset: `CCCR=0x1`, `RXF0S=0`.
- A `reset` macro with `sysbus LoadELF` is required. Without it, PC is 0 and the node crashes. Each machine needs its own macro.

**Disconnect and reconnect (S3).** While disconnected, RX stops and TX stays pending (`TXBRP=0xF`, FIFO full). After reconnect, **TX does not restart on its own**:
- Rewriting `TXBAR` with the pending bits restarts it. This is harmless on real hardware.
- `TXBCR` must not be used: it cancelled 2 of 4 buffers and left the FIFO stuck.
- Stage 3 driver plan: if the FIFO is full, write `TXBAR = TXBRP` and count the event.
- The 4 queued frames go out first after reconnect, so they carry old values.

**UART link.** All 33 lines reached the TCP client, with the client connected before the run. Whether data sent before a client connects is kept was not tested.

## Model limits that affect the design

| Limit | Effect |
|---|---|
| `NBTP`, `DBTP`, `TDCR` writes are dropped and read back as reset values | CONFIG would report wrong bitrates. See below |
| Bit timing not simulated, frames are instant, no arbitration | Bus load and worst-case response stay paper calculations. Renode cannot prove them |
| No ACK or error model on the hub | Error counters and bus-off will not trigger by themselves |
| USART `TXE` is always 1 (from source) | UART output is not paced by baud rate, so the TX overflow policy will not trigger naturally |
| DWT fixed at 250 MHz in the repl (from source, not tested) | Worst loop time in cycles needs this factor, not the core clock |
| GPIO `MODER` resets to `0x00000000`, not the chip's `0xABFFFFFF` (port A) or `0xFFFFFFFF` (others) | Pin setup uses read-modify-write, so our fields are right either way. Whole-register reads differ: PD reads `0x0000000A` in Renode, `0xFFFFFFFA` on the chip |

### Bit-timing readback

`docs/stage0_probe/t4_register_shadow.resc` adds two `sysbus` hooks that store writes to `0x0C`, `0x1C`, `0x48`
per machine and return them on read. The firmware then reads `NBTP=1E013E0F DBTP=00810D44 TDCR=00000F02`,
`CCCR` is unaffected, and two machines stay independent. Firmware stays unchanged and still reads the real registers.

## Reproducing

```
cd docs/stage0_probe && ./build.sh && mkdir -p out
timeout -s KILL 60 renode --disable-gui --console -e "include @$PWD/t2_four_nodes.resc" </dev/null
```

`t2` needs a TCP client on port 3456 to capture the UART link. Logs land in `out/`.
The probe is throwaway test code and is not part of the product firmware.
