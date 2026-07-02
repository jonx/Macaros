ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { ../libc68k/lvo.o(.text) *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) *(.data.rel.ro*) }
  .data : { *(.data) *(.data.*) }
  .bss : { *(.bss) *(.bss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.debug*) }
}
