; crt0.s — minimal C runtime startup + the PutChar LVO shim for the j5m program.
; Hand-written (OURS); NO Amiga SDK, NO real libc.  Assembled by vasm (mot syntax),
; linked with the vbcc-compiled j5m.o by vlink into one AmigaOS hunk executable.
;
; This file MUST be linked FIRST so `_start` is the first code in the CODE hunk: the
; [J4] loader / the engine enter at the first byte of the first code hunk (entry PC),
; and the engine treats the top-level RTS (a7 back at the initial SP it seeded to the
; top of the sandbox) as program exit, with d0 = the exit code.  So _start is a plain
; subroutine: call _main, leave its return in d0, RTS.  (No real Amiga DOS return-code
; convention, no command-line tail — this is a self-contained compute kernel.)
;
; The PutChar shim (_putch) realises the classic AmigaOS library call: arguments arrive
; in named 68k registers and the call is `jsr LVO(a6)` with LVO a NEGATIVE offset
; -n*6 (LIB_VECTSIZE==6; arch/m68k-all/include/aros/cpu.h:81-82, the same rule the
; [J3] bridge + stublib use).  PutChar is exec LVO 5 -> byte offset -30, char in d0.
; A6 already holds the library base (the engine seeds A6 = a6_libbase at entry).
;
; vbcc's C calling convention (verified from its m68k codegen): integer args are
; PUSHED on the stack by the caller (move.l #c,-(a7) ; jsr _putch), so the callee reads
; the first arg at 4(a7) (4 = the return address jsr pushed).  Return value in d0.

LVO_PutChar     equ     -30             ; exec PutChar: -5*6

        section "CODE",code

        public  _start
_start:
        jsr     _main                   ; run the C program
        ; main's int return is already in d0 (vbcc convention) -> the exit code
        rts                             ; top-level RTS = program exit (d0 = exit code)

; void putch(int c);  c is at 4(a7) on entry.  Marshal to d0 and call PutChar(a6).
        public  _putch
_putch:
        move.l  4(a7),d0                ; d0 = c  (PutChar's char arg, exec ABI: D0)
        jsr     LVO_PutChar(a6)         ; jsr -30(a6) -> [J3] bridge -> stub PutChar
        rts
