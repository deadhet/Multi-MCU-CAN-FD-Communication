/* Stage 0 throwaway probe: flash at 0x08000000, data in DTCM. Not product code. */
ENTRY(reset_handler)
MEMORY { FLASH (rx): ORIGIN = 0x08000000, LENGTH = 1024K
         DTCM (rwx): ORIGIN = 0x20000000, LENGTH = 128K }
SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); _etext = .; } > FLASH
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > DTCM AT > FLASH
  .bss  : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > DTCM
  _estack = ORIGIN(DTCM) + LENGTH(DTCM);
}
