| entry_fib.s — call fib(10), exit code = D0. GNU as (m68k-elf) syntax.
	.text
	.globl	_start
_start:
	move.l	#10,-(%sp)
	jsr	fib
	addq.l	#4,%sp
	rts

| compiler_builtins references abort; a trapping stub satisfies the link.
	.globl	abort
abort:
	.word	0x4afc
