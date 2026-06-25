; libcall.s — a 68k program that calls AmigaOS library functions via the standard
; negative-offset LVO mechanism (jsr -offset(a6)). Assembled with
; vasmm68k_mot -Fhunkexe to a REAL big-endian AmigaOS hunk executable.
;
; The classic AmigaOS calling convention: a6 holds the library base, arguments go
; in named 68k registers (d0/d1/a0/a1 ...), and the call is `jsr LVO(a6)` where LVO
; is a NEGATIVE offset  -n*6  (LIB_VECTSIZE==6, __AROS_GETJUMPVEC; grounded in
; arch/m68k-all/include/aros/cpu.h:82,81 and the [J3] bridge). The runner seeds a6
; with the stub exec base.
;
; This program (against the stub exec in apps68k/stublib.c):
;
;   ; --- AllocMem(size=256, flags=MEMF_CLEAR) -> ptr in d0  (LVO -198) ---
;   move.l #256,d0          ; d0 = byteSize        (AROS_LHA reg D0)
;   move.l #1,d1            ; d1 = requirements    (AROS_LHA reg D1)
;   jsr    -198(a6)         ; AllocMem  -> d0 = allocated 68k pointer
;   move.l d0,a2            ; stash the pointer
;
;   ; --- PutChar('A')  (a stub "print", LVO -30) records the byte ---
;   move.l #'A',d0          ; d0 = char
;   jsr    -30(a6)          ; PutChar -> records 'A'
;
;   ; --- FreeMem(ptr, size=256)  (LVO -210) ---
;   move.l a2,a1            ; a1 = memoryBlock     (AROS_LHA reg A1)
;   move.l #256,d0         ; d0 = byteSize        (AROS_LHA reg D0)
;   jsr    -210(a6)         ; FreeMem
;
;   moveq  #0,d0           ; exit code 0
;   rts
;
; DECODER COVERAGE: this exercises the [J3] 68k->native LVO bridge FROM A RUNNING
; PROGRAM — i.e. the decoder must recognise `jsr -offset(a6)`, map it through the
; negative-offset rule to (libbase, LVO n), and invoke the [J3] marshal thunk with
; the An/Dn args marshalled from the sandbox. [J3] proves the bridge in isolation
; (called directly by the test); decoding the jsr-through-vector FROM the
; instruction stream is the "library calls from the running program" item DEFERRED
; to [J5c]. The runner marks this "needs [J5c]" and demonstrates the EXPECTED
; observable effects (AllocMem/FreeMem/PutChar call log) via the stub library +
; the [J3] marshaller driven from a reference decode — it does NOT claim a JIT pass.

LVO_AllocMem    equ     -198
LVO_FreeMem     equ     -210
LVO_PutChar     equ     -30

MEMF_CLEAR      equ     1

    move.l  #256,d0         ; byteSize
    move.l  #MEMF_CLEAR,d1  ; requirements
    jsr     LVO_AllocMem(a6); -> d0 = ptr
    move.l  d0,a2           ; stash

    move.l  #'A',d0         ; char to print
    jsr     LVO_PutChar(a6) ; records 'A'

    move.l  a2,a1           ; memoryBlock
    move.l  #256,d0         ; byteSize
    jsr     LVO_FreeMem(a6) ; free it

    moveq   #0,d0           ; exit 0
    rts
