| lvo.s — crt0 + the stub-DOS LVO call veneers for libc68k. GNU as (m68k-elf).
|
| run68k enters _start with A6 = the stub library base, A0/D0 = the AmigaDOS arg
| string (see run68k.md). crt0 parks them in globals and calls __libc68k_start
| (libc68k.c), which initializes the C runtime and calls the program's exported
| `rust_main68k` — its return value is the exit code (D0 at the final rts).
|
| Each veneer is the ONE real 68k library call for its stub-DOS LVO (stublib.h):
| args in d1/d2/d3, result in d0, negative errno on failure. d2/d3 are callee-saved
| in the m68k C ABI so they are saved around the call; a6 likewise.

	.text
	.globl	_start
_start:
	move.l	%a6,__stub_libbase
	move.l	%a0,__args_ptr
	move.l	%d0,__args_len
	jsr	__libc68k_start
	rts

| compiler_builtins and panic paths reference abort: trap so a fault is diagnosed
| (the engines turn ILLEGAL into a crash bundle, not a silent exit).
	.globl	abort
abort:
	.word	0x4afc

| uint32_t __lvoN(a1 [, a2 [, a3]]) — after movem push (12 bytes) + return address:
| a1 @ 16(sp), a2 @ 20(sp), a3 @ 24(sp). Missing args read caller garbage into the
| unused registers; the host stub only consumes what the LVO's descriptor names.
.macro	LVOCALL name, off
	.globl	\name
\name:
	movem.l	%d2-%d3/%a6,-(%sp)
	move.l	__stub_libbase,%a6
	move.l	16(%sp),%d1
	move.l	20(%sp),%d2
	move.l	24(%sp),%d3
	jsr	\off(%a6)
	movem.l	(%sp)+,%d2-%d3/%a6
	rts
.endm

	LVOCALL	__lvo_open,    -60
	LVOCALL	__lvo_close,   -66
	LVOCALL	__lvo_read,    -72
	LVOCALL	__lvo_write,   -78
	LVOCALL	__lvo_lseek,   -84
	LVOCALL	__lvo_delete,  -90
	LVOCALL	__lvo_mkdir,   -96
	LVOCALL	__lvo_rmdir,   -102
	LVOCALL	__lvo_stat,    -108
	LVOCALL	__lvo_fstat,   -114
	LVOCALL	__lvo_gettime, -120
	LVOCALL	__lvo_entropy, -126

	.bss
	.globl	__stub_libbase
__stub_libbase:
	.space	4
	.globl	__args_ptr
__args_ptr:
	.space	4
	.globl	__args_len
__args_len:
	.space	4
