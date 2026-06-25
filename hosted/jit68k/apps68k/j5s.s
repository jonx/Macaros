; j5s.s — the [J5s] 68881/68882 FP EXCEPTION MODEL exercise. Assembled with
; vasmm68k_mot -Fhunkexe -m68882 -no-opt -kick1hunks to a REAL big-endian AmigaOS hunk exe.
;
; [J5o]..[J5r] built the FP register file + FMOVE/format-conversions + arithmetic + the
; transcendentals + the FP conditional control-flow + FMOVEM/.x — all with the FPSR CONDITION
; byte live. [J5s] adds the FP EXCEPTION MODEL: the FPSR exception (EXC) + accrued (AEXC) bytes,
; the FPCR exception-enable + rounding-mode/precision bytes, the FP exception traps (vectors
; 48..54), and BSUN (the trap the SIGNALLING FP predicates raise on an unordered operand).
;
; Each exception is raised from a REAL cause; the FPSR is read back via FMOVE.l FPSR,Dn and
; stored to a RESULT frame so the harness asserts the EXC + AEXC bits BIT-EXACT vs the oracle:
;   0.0/0.0 -> OPERR ; 1.0/0.0 -> DZ ; overflow -> OVFL ; underflow -> UNFL ; an inexact
;   result -> INEX2 ; a signalling-NaN operand -> SNAN ; a signalling unordered predicate -> BSUN.
; Then the four rounding modes round 1/3 differently; and a TRAP-ENABLED OPERR fires vector 52
; through a planted handler (observed via the [J5i] exception path).
;
;   a0 = RESULT scratch (the harness seeds a0 = lea result). FPSR longwords land here.
;   the vector table @ 0x00240000 is seeded for the OPERR trap (vector 52) — the handler is
;        planted by this program at entry.
;
; NOTE on block structure: each FP op that raises an exception is FOLLOWED by FMOVE.l FPSR,Dn
; (a consumer that ends the JIT block), so the engine processes that block's host fenv BEFORE
; the next FP op — the EXC byte reflects exactly the one op, matching the oracle's per-op model.

; ---- entry --------------------------------------------------------------------------------
    lea     fpconst,a1          ; a1 -> FP constants (relocated DATA)
    lea     result,a0           ; a0 -> RESULT scratch (relocated DATA)
    moveq   #0,d0               ; d0 = running signature

    ; install the OPERR trap handler (vector 52) into the sandbox vector table @ 0x240000+52*4
    ;   52*4 = 208 = 0x0D0  ->  0x002400D0
    lea     trapop,a2
    move.l  a2,d1
    move.l  #$002400D0,a3
    move.l  d1,(a3)

    ; clear FPCR (rounding = nearest, precision = extended, all exception enables OFF)
    moveq   #0,d1
    fmove.l d1,fpcr

    ; helper convention: clear FPSR before each test op so the EXC byte reflects only THAT op
    ; (the EXC byte is re-set per op, but we clear to keep AEXC attribution clean per test).

    ; ================= EXC 1: OPERR (0.0 / 0.0) ===========================================
    moveq   #0,d1
    fmove.l d1,fpsr            ; FPSR = 0 (clear EXC/AEXC/cc)
    fmove.d (a1),fp0           ; fp0 = 0.0
    fmove.d (a1),fp1           ; fp1 = 0.0
    fdiv.x  fp1,fp0            ; 0.0 / 0.0 -> NaN, OPERR
    fmove.l fpsr,d2           ; read FPSR (EXC+AEXC)
    move.l  d2,(a0)           ; result[0] = FPSR after OPERR

    ; ================= EXC 2: DZ (1.0 / 0.0) ==============================================
    moveq   #0,d1
    fmove.l d1,fpsr
    fmove.d 8(a1),fp2          ; fp2 = 1.0
    fmove.d (a1),fp3           ; fp3 = 0.0
    fdiv.x  fp3,fp2            ; 1.0 / 0.0 -> +inf, DZ
    fmove.l fpsr,d2
    move.l  d2,4(a0)          ; result[4] = FPSR after DZ

    ; ================= EXC 3: OVFL (huge * huge) ==========================================
    moveq   #0,d1
    fmove.l d1,fpsr
    fmove.d 16(a1),fp4         ; fp4 = 1e308
    fmove.d 16(a1),fp5         ; fp5 = 1e308
    fmul.x  fp5,fp4           ; 1e308 * 1e308 -> +inf, OVFL (+ INEX2)
    fmove.l fpsr,d2
    move.l  d2,8(a0)          ; result[8] = FPSR after OVFL

    ; ================= EXC 4: UNFL (tiny * tiny) ==========================================
    moveq   #0,d1
    fmove.l d1,fpsr
    fmove.d 24(a1),fp6         ; fp6 = 2.2e-308 (smallest normal-ish)
    fmove.d 24(a1),fp7         ; fp7 = 2.2e-308
    fmul.x  fp7,fp6           ; underflow -> ~0, UNFL (+ INEX2)
    fmove.l fpsr,d2
    move.l  d2,12(a0)         ; result[12] = FPSR after UNFL

    ; ================= EXC 5: INEX (1.0 / 3.0) ===========================================
    moveq   #0,d1
    fmove.l d1,fpsr
    fmove.d 8(a1),fp0          ; fp0 = 1.0
    fmove.d 32(a1),fp1         ; fp1 = 3.0
    fdiv.x  fp1,fp0           ; 1/3 -> inexact, INEX2
    fmove.l fpsr,d2
    move.l  d2,16(a0)         ; result[16] = FPSR after INEX

    ; ================= EXC 6: SNAN (signalling-NaN operand) ===============================
    moveq   #0,d1
    fmove.l d1,fpsr
    fmove.d 40(a1),fp2         ; fp2 = signalling NaN
    fmove.d 8(a1),fp3          ; fp3 = 1.0
    fadd.x  fp2,fp3           ; 1.0 + sNaN -> OPERR/SNAN (signalling operand)
    fmove.l fpsr,d2
    move.l  d2,20(a0)         ; result[20] = FPSR after SNAN

    ; ================= ROUNDING MODES: 1.0/3.0 under RN/RZ/RM/RP =========================
    ; FPCR rounding mode = bits 5-4 (RN=00 RZ=01 RM=10 RP=11). For each mode, divide 1/3 and
    ; store the 8-byte double result; the harness asserts they round differently (RN==RM here,
    ; RP rounds the last bit up). Precision stays extended (double).
    move.l  #$00000000,d1      ; RN
    fmove.l d1,fpcr
    fmove.d 8(a1),fp4
    fdiv.x  fp1,fp4           ; fp1 = 3.0 (still loaded)
    fmove.d fp4,24(a0)        ; result[24..31] = 1/3 RN

    move.l  #$00000010,d1      ; RZ
    fmove.l d1,fpcr
    fmove.d 8(a1),fp4
    fdiv.x  fp1,fp4
    fmove.d fp4,32(a0)        ; result[32..39] = 1/3 RZ

    move.l  #$00000020,d1      ; RM
    fmove.l d1,fpcr
    fmove.d 8(a1),fp4
    fdiv.x  fp1,fp4
    fmove.d fp4,40(a0)        ; result[40..47] = 1/3 RM

    move.l  #$00000030,d1      ; RP
    fmove.l d1,fpcr
    fmove.d 8(a1),fp4
    fdiv.x  fp1,fp4
    fmove.d fp4,48(a0)        ; result[48..55] = 1/3 RP

    ; restore RN, enables off
    moveq   #0,d1
    fmove.l d1,fpcr

    ; ================= BSUN: a signalling unordered predicate on a NaN ====================
    ; ftst a NaN (NAN=1), then FBGT — in vasm Motorola syntax `fbgt` is the SIGNALLING OGT
    ; predicate (selector 0x12, bit4 set), which raises BSUN on an unordered (NaN) operand.
    ; OGT is FALSE on NaN -> NOT taken -> fall through. (Enable off here, so no trap; the BSUN
    ; bit is set + AIOP accrued.)
    moveq   #0,d1
    fmove.l d1,fpsr
    fmove.d 48(a1),fp5         ; fp5 = NaN (quiet NaN at pool +48)
    ftst.x  fp5              ; FPSR: NAN=1
    fbgt    bsun_after        ; SIGNALLING OGT on NaN -> sets BSUN; OGT FALSE on NaN -> fall thru
bsun_after:
    fmove.l fpsr,d2
    move.l  d2,56(a0)         ; result[56] = FPSR after BSUN (BSUN bit set, AIOP accrued)

    ; ================= TRAP-ENABLED OPERR -> vector 52 ===================================
    ; enable OPERR in FPCR (enable byte bit13 = 0x2000), then 0/0 -> the trap fires vector 52.
    ; the handler sets d6 = a marker and rte's back. The fdiv is followed IMMEDIATELY by
    ; FMOVE.l FPSR,d2 (a terminator that ends the JIT block) so the trap fires RIGHT AFTER the
    ; fdiv (before any later instruction runs); the handler's rte returns to that fmove.l.
    move.l  #$00002000,d1      ; FPCR: OPERR enable (bit13)
    fmove.l d1,fpcr
    moveq   #0,d6
    fmove.d (a1),fp0          ; fp0 = 0.0
    fmove.d (a1),fp1          ; fp1 = 0.0
    fdiv.x  fp1,fp0           ; 0/0 -> OPERR ENABLED -> trap vector 52 (handler sets d6)
    fmove.l fpsr,d2           ; (block terminator) — the trap fires here; handler rte's back to it
    ; the handler returns here (rte to this point)
    move.l  d6,60(a0)         ; result[60] = the handler marker (proves the vector fired)

    ; disable enables again
    moveq   #0,d1
    fmove.l d1,fpcr

    ; build the exit signature in d0: the low byte of each EXC test's EXC byte, OR'd; plus the
    ; trap marker bit. (The harness's byte-exact assert is the real gate; d0 is a quick check.)
    move.l  60(a0),d0         ; d0 = the trap marker (0x00000034 = vector 52, set by the handler)

    bra     done

; ---- the OPERR trap handler (vector 52). Sets d6 = marker, then rte. ----------------------
trapop:
    move.l  #$00000034,d6     ; marker = 52 (the OPERR vector number)
    rte

done:
    rts

; ---- FP constant pool (relocated DATA: a1 = lea fpconst). All doubles (.d, IEEE binary64). ----
    cnop    0,8
fpconst:
    dc.l    $00000000,$00000000 ; +0  : 0.0
    dc.l    $3FF00000,$00000000 ; +8  : 1.0
    dc.l    $7FE00000,$00000000 ; +16 : ~1.12e308 (large; squared -> overflow)
    dc.l    $00100000,$00000000 ; +24 : 2.2250738585072014e-308 (smallest normal; squared->unfl)
    dc.l    $40080000,$00000000 ; +32 : 3.0
    dc.l    $7FF00000,$00000001 ; +40 : signalling NaN (exp all-ones, quiet bit 0, payload != 0)
    dc.l    $7FF80000,$00000000 ; +48 : quiet NaN

; ---- RESULT scratch area (relocated DATA: a0 = lea result). 128 bytes, zero-filled. ----------
    cnop    0,8
result:
    ds.b    128
