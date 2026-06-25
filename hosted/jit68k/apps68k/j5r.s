; j5r.s — the [J5r] FMOVEM + FP SYSTEM-REGISTER MOVES + the 80-bit extended (.x) memory format.
; Assembled with vasmm68k_mot -Fhunkexe -m68882 -no-opt -kick1hunks to a REAL big-endian hunk exe.
;
; The last decoder-level FP gap before the capstone. Covers:
;   FMOVEM.x <reglist>,-(An)   (predecrement SAVE — the FP function prologue)
;   FMOVEM.x (An)+,<reglist>   (postincrement RESTORE — the epilogue)
;   FMOVEM.x <reglist>,(An)    (control-mode store)  + FMOVEM.x (An),<reglist> (control load)
;   FMOVE.l Dn,FPCR / FPCR,Dn ; FMOVE.l Dn,FPSR / FPSR,Dn ; FMOVE.l Dn,FPIAR / FPIAR,Dn
;   FMOVEM.l FPCR/FPSR,(An)    (multiple control regs in one instruction)
; FP registers in memory are in the 96-bit EXTENDED (.x) format (12 bytes: sign+15-bit exp,
; reserved word, 64-bit mantissa with explicit integer bit). Our FP regs are IEEE double, so
; double->.x->double round-trips EXACTLY (the gate). FMOVEM memory is sandbox-routed; the test
; asserts the FP regs + the extended-format memory bytes + FPCR/FPSR/FPIAR byte-exact vs the oracle.
;
;   a0 = a sandbox scratch frame (the test seeds a0); a7 = the supervisor stack (FMOVEM -(sp)).
;   The function `clobberer` saves fp0-fp7 to -(sp), clobbers them all, restores from (sp)+,
;   and returns — so the CALLER's fp0-fp7 must SURVIVE intact (the prologue/epilogue contract).

; ---- entry --------------------------------------------------------------------------------
    lea     fpconst,a1          ; a1 -> FP constants (relocated DATA)
    lea     scratch,a0          ; a0 -> scratch frame (relocated DATA)
    moveq   #0,d0

    ; load fp0..fp7 with distinct known values
    fmove.d (a1),fp0           ; 1.0
    fmove.d 8(a1),fp1          ; 2.0
    fmove.d 16(a1),fp2         ; 3.0
    fmove.d 24(a1),fp3         ; -4.5
    fmove.d 32(a1),fp4         ; 0.5
    fmove.d 40(a1),fp5         ; 100.0
    fmove.d 48(a1),fp6         ; -0.0
    fmove.d 56(a1),fp7         ; 1234.5

    ; ---- call the clobberer: it must save+restore fp0-fp7, so they SURVIVE ----------------
    bsr     clobberer

    ; ---- assert survival by storing fp0-fp7 to the scratch frame as .x (the test reads them) -
    fmovem.x fp0-fp7,(a0)      ; control-mode store: 8 * 12 = 96 bytes at scratch[0..95]

    ; ---- FPCR / FPSR / FPIAR round-trip via Dn ---------------------------------------------
    move.l  #$00000030,d1      ; a test FPCR pattern (rounding/precision bits)
    fmove.l d1,fpcr            ; FPCR <- d1
    fmove.l fpcr,d2            ; d2 <- FPCR (must == d1)
    move.l  d2,96(a0)          ; store the read-back FPCR at scratch+96

    move.l  #$08000000,d3      ; an FPSR pattern (N condition bit set, bit27)
    fmove.l d3,fpsr            ; FPSR <- d3
    fmove.l fpsr,d4            ; d4 <- FPSR
    move.l  d4,100(a0)         ; store the read-back FPSR at scratch+100

    move.l  #$00210ABC,d5      ; an FPIAR pattern (a 68k address-like value)
    fmove.l d5,fpiar           ; FPIAR <- d5
    fmove.l fpiar,d6           ; d6 <- FPIAR
    move.l  d6,104(a0)         ; store the read-back FPIAR at scratch+104

    ; ---- FMOVEM.l of MULTIPLE control regs in one instruction: FPCR + FPSR -> memory --------
    fmovem.l fpcr/fpsr,108(a0) ; stores FPCR @ +108, FPSR @ +112 (4 bytes each)

    ; build a small exit signature: d0 = 1 if everything ran (the real proof is byte-exact)
    moveq   #1,d0
    rts

; ---- the clobberer: an FP-using subroutine that preserves fp0-fp7 across its body ----------
clobberer:
    fmovem.x fp0-fp7,-(sp)     ; SAVE all 8 FP regs (predecrement, 96 bytes) — the prologue
    ; clobber every FP register with garbage
    fmove.d 64(a1),fp0         ; 9999.0
    fmove.x fp0,fp1
    fmove.x fp0,fp2
    fmove.x fp0,fp3
    fmove.x fp0,fp4
    fmove.x fp0,fp5
    fmove.x fp0,fp6
    fmove.x fp0,fp7
    fmovem.x (sp)+,fp0-fp7     ; RESTORE all 8 FP regs (postincrement) — the epilogue
    rts

; ---- FP constant pool (relocated DATA: a1 = lea fpconst). All doubles (.d, IEEE binary64). ----
    cnop    0,8
fpconst:
    dc.l    $3FF00000,$00000000 ; +0  : 1.0
    dc.l    $40000000,$00000000 ; +8  : 2.0
    dc.l    $40080000,$00000000 ; +16 : 3.0
    dc.l    $C0120000,$00000000 ; +24 : -4.5
    dc.l    $3FE00000,$00000000 ; +32 : 0.5
    dc.l    $40590000,$00000000 ; +40 : 100.0
    dc.l    $80000000,$00000000 ; +48 : -0.0
    dc.l    $40934A00,$00000000 ; +56 : 1234.5
    dc.l    $40C387C0,$00000000 ; +64 : 9999.0   (the clobber value)

; ---- scratch frame (relocated DATA: a0 = lea scratch). 128 bytes, zero-filled. --------------
    cnop    0,8
scratch:
    ds.b    128
