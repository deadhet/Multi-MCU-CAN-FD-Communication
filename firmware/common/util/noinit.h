/* NOINIT_RAM: puts a variable in the .noinit section, in AXI SRAM at 0x24000000 (see
 * firmware/linker/stm32h743vit6.ld).
 *
 * - Start-up does not clear it: the startup code clears only .bss (in DTCM), so big buffers here
 *   do not make a node start later than the others.
 * - DMA1 and DMA2 can reach it; they cannot reach DTCM, where everything else lives.
 * - After a reset it holds whatever was there before. Use it only for buffers that are always
 *   written before they are read.
 * - The D-cache is off in this firmware, so the CPU and a DMA see the same bytes. If the D-cache
 *   is ever turned on, the MPU must mark this region non-cacheable first.
 *
 * On the PC (host tests) it does nothing: the variable is ordinary static data. */
#ifndef NOINIT_H
#define NOINIT_H

#if defined(__arm__)
#define NOINIT_RAM __attribute__((section(".noinit")))
#else
#define NOINIT_RAM
#endif

#endif
