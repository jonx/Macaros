| entry_hello.s — _start passes the stub library base (A6, seeded by run68k) to the
| Rust `hello(libbase)`, and provides putchar68k(c, libbase): the one real 68k
| library call (PutChar, LVO 5 = offset -30, d0 = char). GNU as (m68k-elf) syntax.
	.text
	.globl	_start
_start:
	move.l	%a6,-(%sp)
	jsr	hello
	addq.l	#4,%sp
	rts

| void putchar68k(u32 c, u32 libbase) — C ABI, stack args. After the a6 push:
| 4(sp)=return, 8(sp)=c, 12(sp)=libbase. a6 is callee-saved, so save/restore it.
	.globl	putchar68k
putchar68k:
	move.l	%a6,-(%sp)
	move.l	12(%sp),%a6
	move.l	8(%sp),%d0
	jsr	-30(%a6)
	move.l	(%sp)+,%a6
	rts

| compiler_builtins references abort; a trapping stub satisfies the link.
	.globl	abort
abort:
	.word	0x4afc
