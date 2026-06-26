; crt0_fp.s — C runtime startup + the PutChar LVO shim + the THREE hardware-68881
; transcendental shims, for the [J5t] FP capstone (apps68k/j5t.c).  Hand-written (OURS);
; NO Amiga SDK, NO real libc, NO libm.  Assembled by vasm (mot syntax, -m68882), linked
; with the vbcc-compiled j5t.o by vlink into one AmigaOS hunk executable.
;
; Like crt0.s, this MUST be linked FIRST so `_start` is the first code in the CODE hunk:
; the [J4] loader / the engine enter at the first byte of the first code hunk (entry PC),
; and the engine treats the top-level RTS (a7 back at the initial SP it seeded to the top
; of the sandbox) as program exit, with d0 = the exit code.  So _start is a plain
; subroutine: call _main, leave its int return in d0, RTS.
;
; PutChar shim (_putch) — identical to crt0.s: the classic AmigaOS negative-offset LVO
; call `jsr -30(a6)`, char in d0.  A6 already holds the library base (the engine seeds it).
; The vbcc convention pushes the int arg on the stack, so _putch reads it at 4(a7).
;
; THE HARDWARE-FP TRANSCENDENTAL SHIMS (_sqrt/_sin/_exp) — this is the FP capstone's point:
; the vbcc-compiled C calls sqrt()/sin()/exp() with `jsr _name` and the AmigaOS FP calling
; convention vbcc emits (verified from its m68k -fpu=68881 codegen):
;   * the double ARGUMENT is pushed on the stack (8 bytes, big-endian) -> at 4(a7) in the
;     callee (4 = the jsr return address);
;   * the double RESULT is returned in fp0 (vbcc's __fp0ret convention).
; Each shim loads the arg into fp0 and applies the actual 68881 HARDWARE transcendental
; opcode, so the COMPILER'S call lands on a real line-F op the JIT decodes:
;   _sqrt -> FSQRT.x   (the [J5o] core unary op)
;   _sin  -> FSIN.x    (the [J5p] transcendental)
;   _exp  -> FETOX.x   (e^x, the [J5p] transcendental)
; fp0 is caller-saved here (vbcc reloads its FP regs from the fmovem.x prologue), so the
; shims need no fmovem save/restore — they are leaf, single-op routines.

LVO_PutChar     equ     -30             ; exec PutChar: -5*6

        section "CODE",code

        public  _start
_start:
        jsr     _main                   ; run the C program
        rts                             ; top-level RTS = program exit (d0 = exit code)

; void putch(int c);  c is at 4(a7) on entry.  Marshal to d0 and call PutChar(a6).
        public  _putch
_putch:
        move.l  4(a7),d0                ; d0 = c (PutChar's char arg, exec ABI: D0)
        jsr     LVO_PutChar(a6)         ; jsr -30(a6) -> [J3] bridge -> stub PutChar
        rts

; double sqrt(double x);  x at 4(a7); result in fp0.  HARDWARE FSQRT.
        public  _sqrt
_sqrt:
        fmove.d 4(a7),fp0               ; fp0 = x  (double from the stack)
        fsqrt.x fp0                     ; fp0 = sqrt(fp0)  — HARDWARE 68881 FSQRT
        rts

; double sin(double x);  x at 4(a7); result in fp0.  HARDWARE FSIN.
        public  _sin
_sin:
        fmove.d 4(a7),fp0               ; fp0 = x
        fsin.x  fp0                     ; fp0 = sin(fp0)   — HARDWARE 68881 FSIN
        rts

; double exp(double x);  x at 4(a7); result in fp0.  HARDWARE FETOX (e^x).
        public  _exp
_exp:
        fmove.d 4(a7),fp0               ; fp0 = x
        fetox.x fp0                     ; fp0 = e^fp0      — HARDWARE 68881 FETOX
        rts
