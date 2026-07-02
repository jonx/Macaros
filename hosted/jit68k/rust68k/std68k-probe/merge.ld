SECTIONS {
  .text : { *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) *(.data.rel.ro*) }
  .data : { *(.data) *(.data.*) }
  .bss : { *(.bss) *(.bss.*) *(COMMON) }
}
