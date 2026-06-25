; j5o.s — the [J5o] 68881/68882 FPU CORE exercise. Assembled with
; vasmm68k_mot -Fhunkexe -m68882 -no-opt to a REAL big-endian AmigaOS hunk executable.
;
; This is the FIRST program in the corpus to use the FPU coprocessor (line-F). It drives
; the REAL Emu68 EMIT_FPU decoder through the [J5o] engine and is asserted BYTE-EXACT (the
; FP0..FP7 double bit patterns + the FPSR condition byte + the FP-to-memory results) vs the
; INDEPENDENT C-double oracle (j5d_interp.c, OURS, no Emu68).
;
; COVERS this increment's ops, each at IEEE double precision (the precision model: FP regs
; are modeled in binary64; 80-bit extended exactness is not bit-reproducible on AArch64):
;   FMOVE  — integer .l/.w/.b -> FP ; single .s & double .d memory <-> FP ; FP reg <-> reg
;   FADD FSUB FMUL FDIV  — the four IEEE arithmetic ops, FPn op <src>
;   FSQRT FABS FNEG      — the unary ops
;   FCMP FTST            — set the FPSR condition codes (asserted via a following FMOVE->MEM,
;                          which forces FPSR_Update_Needed so the cc byte is computed)
;
; The program builds a small computation whose FP register file + the stored doubles are a
; deterministic function of every op, and returns a derived integer in d0 (a coarse exit
; signature; the REAL proof is the byte-exact FP-state + memory + FPSR assert in the test).
;
;   a0 = RESULT scratch area (the test seeds a0 with a sandbox data address)
;
; the FP work (all results land in FP regs + the RESULT area):
;   fp0 = (double) 7          (fmove.l)             = 7.0
;   fp1 <- 2.5  (.s single from mem)                = 2.5
;   fp2 <- 10.0 (.d double from mem)                = 10.0
;   fp0 = fp0 + fp1           (fadd)                = 9.5
;   fp0 = fp0 * fp2           (fmul)                = 95.0
;   fp3 = fp0                 (fmove reg->reg)      = 95.0
;   fp3 = fp3 - fp1           (fsub)                = 92.5
;   fp4 = fp2 / fp1           (fdiv 10/2.5)         = 4.0
;   fp4 = fsqrt fp4          (fsqrt -> 2.0)         = 2.0
;   fp5 <- -3   (.w int -> fp, negative)            = -3.0
;   fp5 = fabs fp5           (fabs)                 = 3.0
;   fp6 = fneg fp4          (fneg 2.0 -> -2.0)      = -2.0
;   fp7 <- 100  (.b int -> fp)                      = 100.0
;   fcmp fp1,fp0  (95.0 ? 2.5)  then store fp0.d    ; FPSR: 95>2.5 -> N=0 Z=0
;   ftst fp6      (-2.0)         then store fp6.d    ; FPSR: -2<0  -> N=1 Z=0
;   store fp3.s (92.5 single), fp4.l (2 int), fp5.w (3), fp7.b (100) to RESULT
;   d0 = 0 (exit ok; the assert is byte-exact, not the exit code)

; ---- entry --------------------------------------------------------------------------------
    lea     fpconst,a1          ; a1 -> the FP constant pool (relocated DATA)
    lea     result,a0           ; a0 -> the RESULT scratch area (relocated DATA)

    moveq   #7,d0
    fmove.l d0,fp0              ; fp0 = 7.0                       (int .l -> FP)
    fmove.s (a1),fp1           ; fp1 = 2.5  (single .s from mem) (format-convert load)
    fmove.d 4(a1),fp2          ; fp2 = 10.0 (double .d from mem)

    fadd.x  fp1,fp0            ; fp0 = 7.0 + 2.5 = 9.5
    fmul.x  fp2,fp0            ; fp0 = 9.5 * 10.0 = 95.0

    fmove.x fp0,fp3            ; fp3 = 95.0       (FP reg -> reg)
    fsub.x  fp1,fp3            ; fp3 = 95.0 - 2.5 = 92.5

    fmove.x fp2,fp4            ; fp4 = 10.0
    fdiv.x  fp1,fp4            ; fp4 = 10.0 / 2.5 = 4.0
    fsqrt.x fp4,fp4            ; fp4 = sqrt(4.0) = 2.0

    moveq   #-3,d1
    fmove.w d1,fp5             ; fp5 = -3.0       (int .w sign-extended -> FP)
    fabs.x  fp5,fp5            ; fp5 = |-3.0| = 3.0

    fneg.x  fp4,fp6            ; fp6 = -(2.0) = -2.0

    moveq   #100,d2
    fmove.b d2,fp7             ; fp7 = 100.0      (int .b -> FP)

    ; ---- FCMP / FTST, each followed by an FMOVE->MEM so the FPSR cc byte is computed ----
    fcmp.x  fp1,fp0            ; compare fp0(95.0) ? fp1(2.5)  -> FPSR set (95 > 2.5)
    fmove.d fp0,(a0)           ; store fp0 (95.0) as double  [RESULT+0]   (consumes FPSR)

    ftst.x  fp6               ; test fp6 (-2.0)               -> FPSR set (N=1)
    fmove.d fp6,8(a0)         ; store fp6 (-2.0) as double  [RESULT+8]   (consumes FPSR)

    ; ---- format-conversion STORES (FP -> mem, narrowing) -------------------------------
    fmove.s fp3,16(a0)        ; store fp3 (92.5) as single  [RESULT+16]
    fmove.l fp4,20(a0)        ; store fp4 (2.0) as int .l   [RESULT+20]  = 2
    fmove.w fp5,24(a0)        ; store fp5 (3.0) as int .w   [RESULT+24]  = 3
    fmove.b fp7,26(a0)        ; store fp7 (100.0) as int .b [RESULT+26]  = 100

    moveq   #0,d0             ; exit signature (the assert is the byte-exact FP state + memory)
    rts

; ---- FP constant pool (relocated DATA: a1 = lea fpconst) ----------------------------------
    cnop    0,4
fpconst:
    dc.l    $40200000          ; +0  : single 2.5  (IEEE binary32 0x40200000)
    dc.l    $40240000,$00000000 ; +4 : double 10.0 (IEEE binary64 0x4024000000000000)

; ---- the RESULT scratch area (relocated DATA: a0 = lea result). 32 bytes, zero-filled. -----
    cnop    0,8
result:
    dc.l    0,0                 ; +0  : fp0 stored .d   (95.0)
    dc.l    0,0                 ; +8  : fp6 stored .d   (-2.0)
    dc.l    0                   ; +16 : fp3 stored .s   (92.5)
    dc.l    0                   ; +20 : fp4 stored .l   (2)
    dc.w    0                   ; +24 : fp5 stored .w   (3)
    dc.b    0,0                 ; +26 : fp7 stored .b   (100), +27 pad
    dc.l    0                   ; +28 : tail pad
