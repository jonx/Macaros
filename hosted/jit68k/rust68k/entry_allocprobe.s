| entry_allocprobe.s — call allocprobe(), exit code = D0. GNU as (m68k-elf) syntax.
	.text
	.globl	_start
_start:
	jsr	allocprobe
	rts

| compiler_builtins references abort; a trapping stub satisfies the link.
	.globl	abort
abort:
	.word	0x4afc
