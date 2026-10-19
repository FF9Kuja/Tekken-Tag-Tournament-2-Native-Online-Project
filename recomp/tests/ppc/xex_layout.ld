/* Layout for license-free PowerPC test programs that scripts/ppc_elf_to_xex.py wraps into XEX2 images:
   image base 0x82000000, first 64 KiB left free for the PE headers, one 64 KiB page per section kind. */
ENTRY(_start)
SECTIONS {
  . = 0x82010000;
  .text : { *(.text .text.*) }
  . = ALIGN(0x10000);
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(0x10000);
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
