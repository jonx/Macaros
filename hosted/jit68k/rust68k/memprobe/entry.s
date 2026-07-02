	.text
	.globl	_start
_start:
	move.l	%a6,-(%sp)
	jsr	memprobe
	addq.l	#4,%sp
	rts
	.globl	putchar68k
putchar68k:
	move.l	%a6,-(%sp)
	move.l	12(%sp),%a6
	move.l	8(%sp),%d0
	jsr	-30(%a6)
	move.l	(%sp)+,%a6
	rts
	.globl	abort
abort:
	.word	0x4afc
