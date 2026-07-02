	.text
	.globl	_start
_start:
	jsr	casprobe
	rts
	.globl	abort
abort:
	.word	0x4afc
