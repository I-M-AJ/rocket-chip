OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  DRAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64K
}

SECTIONS
{
  . = 0x80000000;

  .text.init : {
    *(.text.init)
  } > DRAM

  . = ALIGN(64);
  .tohost : {
    *(.tohost)
  } > DRAM

  . = ALIGN(16);
  .text : {
    *(.text .text.*)
  } > DRAM

  . = ALIGN(16);
  .rodata : {
    *(.rodata .rodata.* .srodata .srodata.*)
  } > DRAM

  . = ALIGN(16);
  .data : {
    *(.data .data.* .sdata .sdata.*)
  } > DRAM

  . = ALIGN(16);
  .bss : {
    __bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > DRAM

  . = ALIGN(16);
  . += 0x4000;
  _stack_top = .;
}
