; diagill.s — [J5n] a 68k program that executes an ILLEGAL instruction with no handler
; installed, so it is an UNRECOVERABLE 68k fault (vector 4) -> the diagnostics bundle.
; Assembled to a big-endian AmigaOS hunk executable.
;
;   start:
;       moveq #7,d0
;       illegal              ; 0x4AFC -> vector 4, no handler -> bundle
;       rts                  ; not reached

    moveq   #7,d0
    illegal
    rts
