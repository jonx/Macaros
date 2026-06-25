; diagfault.s — [J5n] a small LABELLED 68k program that FAULTS deterministically, used to
; exercise the diagnostics crash bundle (symbol mapping, the 68k call-stack walk, the fault
; coordinate). Assembled WITHOUT -nosym so it carries a HUNK_SYMBOL hunk (label -> offset),
; so the crash report can name the faulting function and the caller.
;
; The program: main sets up operands, BSRs into `divide`, which performs a divu.w by ZERO.
; No vector-5 (divide-by-zero) handler is installed, so this is an UNRECOVERABLE 68k fault —
; exactly the "never silent" case the [J5n] bundle must catch. The fault lands at a known
; instruction inside `divide`, called from a known site in `main`, so the report's coordinate,
; the 68k call stack, and the symbol names are all checkable.
;
;   main:
;       moveq #100,d0        ; dividend
;       moveq #0,d1          ; (the divisor word is the immediate #0 below)
;       bsr   divide         ; <- the call site recorded on the 68k return stack
;       rts                  ; (never reached — divide faults)
;   divide:
;       divu.w #0,d0         ; <- THE FAULT: divide by zero, no handler -> bundle
;       rts
;
; Assembled to a big-endian AmigaOS hunk executable by apps68k/tools (no -nosym).

    moveq   #100,d0         ; main: dividend in d0
    moveq   #0,d1           ; (scratch)
    bsr     divide          ; call divide -> faults inside it
    rts                     ; not reached

divide:
    divu.w  #0,d0           ; divide by zero -> 68k vector 5, no handler -> unrecoverable
    rts                     ; not reached
