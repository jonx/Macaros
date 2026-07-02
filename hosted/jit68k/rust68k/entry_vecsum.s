| entry_vecsum.s — call vecsum(), exit code = D0. GNU as (m68k-elf) syntax.
	.text
	.globl	_start
_start:
	jsr	vecsum
	rts

| compiler_builtins references abort; a trapping stub satisfies the link.
	.globl	abort
abort:
	.word	0x4afc
