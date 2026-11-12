/* STM32H743VIT6: 2 MB flash, 128 KB DTCM, 512 KB AXI SRAM. All data and the stack live in
 * DTCM, except .noinit (util/noinit.h), which lives in AXI SRAM.
 * There is no heap section: dynamic allocation is not allowed in this project. */
ENTRY(Reset_Handler)

MIN_STACK_SIZE = 4K;

MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  DTCM     (rw)  : ORIGIN = 0x20000000, LENGTH = 128K
  AXI_SRAM (rw)  : ORIGIN = 0x24000000, LENGTH = 512K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

  .node_id : { KEEP(*(.node_id)) } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  .ARM.exidx : { *(.ARM.exidx*) } > FLASH

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > DTCM AT > FLASH

  _sidata = LOADADDR(.data);

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > DTCM

  /* Not cleared at start-up (no symbols for the startup code) and not in the flash image
   * (NOLOAD): buffers that are always written before they are read, and that a DMA must reach.
   * After a reset they hold old contents. */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit*)
    . = ALIGN(4);
  } > AXI_SRAM

  ASSERT(_ebss + MIN_STACK_SIZE <= _estack, "DTCM overflow: no room left for the stack")
}
