; j5p.s — the [J5p] 68881/68882 TRANSCENDENTAL + FP-UTILITY exercise. Assembled with
; vasmm68k_mot -Fhunkexe -m68882 -no-opt -kick1hunks to a REAL big-endian AmigaOS hunk exe.
;
; [J5o] landed the FP CORE (FMOVE/format-convert/FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG/FCMP/FTST).
; [J5p] adds the 68881 TRANSCENDENTALS and the FP-UTILITY ops, driving the REAL Emu68 EMIT_FPU
; decoder (M68k_LINEF.c, verbatim) and asserting BIT-EXACT vs the INDEPENDENT C-double oracle
; (j5d_interp.c). Both sides use the SAME host libm as the reference, so the assert verifies the
; TRANSLATION (correct decode + correct register-as-argument + correct store), not a re-derivation
; of sin(). Results are IEEE binary64 (the [J5o] precision model; 80-bit extended is not
; bit-reproducible on AArch64).
;
; COVERS, each unary fp_dst = fn(src) unless noted (the FPU command-word opmode selects fn):
;   FSIN FCOS FTAN FASIN FACOS FATAN          (trig + inverse trig)
;   FSINH FCOSH FTANH FATANH                  (hyperbolic + inverse hyperbolic)
;   FETOX(e^x) FETOXM1(e^x-1) FTWOTOX(2^x) FTENTOX(10^x)   (exponentials)
;   FLOGN(ln) FLOGNP1(ln(1+x)) FLOG10 FLOG2   (logarithms)
;   FSINCOS  -> sin INTO one FP reg AND cos INTO another (two results)
;   FINT     -> round to integral (FPCR mode; host default round-nearest-even)
;   FINTRZ   -> round toward zero (truncate)
;   FGETEXP  -> the unbiased binary exponent as a double
;   FGETMAN  -> the mantissa forced into [1,2)
;   FMOD     -> x - y*trunc(x/y)
;   FREM     -> IEEE round-to-nearest remainder
;   FSCALE   -> fp_dst * 2^trunc(src)
; plus EDGE CASES that exercise the FPSR NAN/I bits:
;   FACOS(2.0)  -> NaN   (domain error)  ; FLOGN(-1.0) -> NaN ; FATANH(2.0) -> NaN
; each NaN result is consumed by an FMOVE->MEM so the FPSR cc byte (NAN/I) is computed + asserted.
;
;   a0 = RESULT scratch area (the test seeds a0 with a sandbox data address); each op stores its
;        fp_dst as a double (.d) so the byte-exact memory assert covers the stored results too.

; ---- entry --------------------------------------------------------------------------------
    lea     fpconst,a1          ; a1 -> the FP constant pool (relocated DATA)
    lea     result,a0           ; a0 -> the RESULT scratch area (relocated DATA)

    ; ---- TRIG: src = 0.5 (fp0) -------------------------------------------------------------
    fmove.d (a1),fp0           ; fp0 = 0.5
    fsin.x  fp0,fp1            ; fp1 = sin(0.5)
    fmove.d fp1,0(a0)
    fcos.x  fp0,fp2            ; fp2 = cos(0.5)
    fmove.d fp2,8(a0)
    ftan.x  fp0,fp3            ; fp3 = tan(0.5)
    fmove.d fp3,16(a0)

    ; ---- INVERSE TRIG: src = 0.5 -----------------------------------------------------------
    fasin.x fp0,fp4           ; fp4 = asin(0.5)
    fmove.d fp4,24(a0)
    facos.x fp0,fp5           ; fp5 = acos(0.5)
    fmove.d fp5,32(a0)
    fatan.x fp0,fp6           ; fp6 = atan(0.5)
    fmove.d fp6,40(a0)

    ; ---- HYPERBOLIC: src = 0.5 -------------------------------------------------------------
    fsinh.x fp0,fp1           ; fp1 = sinh(0.5)
    fmove.d fp1,48(a0)
    fcosh.x fp0,fp2           ; fp2 = cosh(0.5)
    fmove.d fp2,56(a0)
    ftanh.x fp0,fp3           ; fp3 = tanh(0.5)
    fmove.d fp3,64(a0)
    fatanh.x fp0,fp4          ; fp4 = atanh(0.5)
    fmove.d fp4,72(a0)

    ; ---- EXPONENTIALS: src = 1.0 (fp7) -----------------------------------------------------
    fmove.d 8(a1),fp7         ; fp7 = 1.0
    fetox.x fp7,fp1           ; fp1 = e^1.0
    fmove.d fp1,80(a0)
    fetoxm1.x fp7,fp2         ; fp2 = e^1.0 - 1
    fmove.d fp2,88(a0)
    ftwotox.x fp7,fp3         ; fp3 = 2^1.0
    fmove.d fp3,96(a0)
    ftentox.x fp7,fp4         ; fp4 = 10^1.0
    fmove.d fp4,104(a0)

    ; ---- LOGARITHMS: src = 10.0 (fp7) ------------------------------------------------------
    fmove.d 16(a1),fp7        ; fp7 = 10.0
    flogn.x fp7,fp1           ; fp1 = ln(10.0)
    fmove.d fp1,112(a0)
    flognp1.x fp7,fp2         ; fp2 = ln(1+10.0)
    fmove.d fp2,120(a0)
    flog10.x fp7,fp3          ; fp3 = log10(10.0) = 1.0
    fmove.d fp3,128(a0)
    flog2.x fp7,fp4           ; fp4 = log2(10.0)
    fmove.d fp4,136(a0)

    ; ---- FSINCOS: src = 0.5 -> sin into fp5, cos into fp6 ----------------------------------
    fsincos.x fp0,fp6:fp5     ; fp5 = sin(0.5), fp6 = cos(0.5)   (fp_dst_sin=fp5, fp_dst_cos=fp6)
    fmove.d fp5,144(a0)       ; stored sin
    fmove.d fp6,152(a0)       ; stored cos

    ; ---- FP-UTILITY ------------------------------------------------------------------------
    fmove.d 24(a1),fp7        ; fp7 = 2.5
    fint.x   fp7,fp1          ; fp1 = round(2.5) = 2.0 (nearest-even)
    fmove.d fp1,160(a0)
    fintrz.x fp7,fp2          ; fp2 = trunc(2.5) = 2.0
    fmove.d fp2,168(a0)

    fmove.d 32(a1),fp7        ; fp7 = 12.0
    fgetexp.x fp7,fp3         ; fp3 = exponent(12.0) = 3.0 (1.5 * 2^3)
    fmove.d fp3,176(a0)
    fgetman.x fp7,fp4         ; fp4 = mantissa(12.0) = 1.5
    fmove.d fp4,184(a0)

    ; FMOD / FREM : fp_dst MOD/REM src. fp_dst = 7.0, src = 3.0
    fmove.d 40(a1),fp5        ; fp5 = 7.0
    fmove.d 48(a1),fp6        ; fp6 = 3.0
    fmod.x  fp6,fp5           ; fp5 = 7.0 mod 3.0 = 1.0
    fmove.d fp5,192(a0)
    fmove.d 40(a1),fp5        ; fp5 = 7.0 (reload, fmod clobbered it)
    frem.x  fp6,fp5           ; fp5 = remainder(7.0,3.0) = 1.0
    fmove.d fp5,200(a0)

    ; FSCALE : fp_dst * 2^trunc(src). fp_dst = 1.5, src = 3.0 -> 12.0
    fmove.d 32(a1),fp1        ; (reuse 12.0 area irrelevant) -- load 1.5 below instead
    fmove.d 56(a1),fp1        ; fp1 = 1.5
    fmove.d 48(a1),fp2        ; fp2 = 3.0 (the scale exponent)
    fscale.x fp2,fp1          ; fp1 = 1.5 * 2^3 = 12.0
    fmove.d fp1,208(a0)

    ; ---- EDGE CASES (FPSR NAN/I bits) : domain errors -> NaN, consumed by FMOVE->MEM -------
    fmove.d (a1),fp0          ; fp0 = 0.5 (reset src)
    fmove.d 16(a1),fp7        ; fp7 = 10.0
    facos.x fp7,fp1           ; fp1 = acos(10.0) = NaN  (|x|>1 domain error)
    fmove.d fp1,216(a0)       ; consume FPSR -> NAN/I bits set
    fmove.d 64(a1),fp7        ; fp7 = -1.0
    flogn.x fp7,fp2           ; fp2 = ln(-1.0) = NaN
    fmove.d fp2,224(a0)       ; consume FPSR -> NAN/I bits set
    fmove.d 16(a1),fp7        ; fp7 = 10.0
    fatanh.x fp7,fp3          ; fp3 = atanh(10.0) = NaN  (|x|>1)
    fmove.d fp3,232(a0)       ; consume FPSR -> NAN/I bits set  (LAST FPSR-setting op)

    moveq   #0,d0             ; exit signature (the assert is the byte-exact FP state + memory)
    rts

; ---- FP constant pool (relocated DATA: a1 = lea fpconst). All doubles (.d, IEEE binary64). ----
    cnop    0,8
fpconst:
    dc.l    $3FE00000,$00000000 ; +0  : 0.5
    dc.l    $3FF00000,$00000000 ; +8  : 1.0
    dc.l    $40240000,$00000000 ; +16 : 10.0
    dc.l    $40040000,$00000000 ; +24 : 2.5
    dc.l    $40280000,$00000000 ; +32 : 12.0
    dc.l    $401C0000,$00000000 ; +40 : 7.0
    dc.l    $40080000,$00000000 ; +48 : 3.0
    dc.l    $3FF80000,$00000000 ; +56 : 1.5
    dc.l    $BFF00000,$00000000 ; +64 : -1.0

; ---- the RESULT scratch area (relocated DATA: a0 = lea result). 30 doubles, zero-filled. ------
    cnop    0,8
result:
    ds.b    256                 ; 32 stored doubles' worth of scratch (we use 30 = 240 bytes)
