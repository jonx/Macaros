; j5q.s — the [J5q] 68881/68882 FP CONDITIONAL CONTROL-FLOW exercise. Assembled with
; vasmm68k_mot -Fhunkexe -m68882 -no-opt -kick1hunks to a REAL big-endian AmigaOS hunk exe.
;
; [J5o]/[J5p] made the FPSR condition byte (N/Z/NAN) live + verified bit-exact. [J5q] adds the
; 68881 FP CONDITIONAL CONTROL-FLOW family — FBcc, FScc, FDBcc, FTRAPcc — which read that FPSR
; cc and branch/set/trap on the FP predicate. These are decoded at the DISPATCHER level in C
; (the way integer Bcc is), evaluating the 68881 FP predicate over {N,Z,NAN}; verified byte-exact
; + correct control flow vs the INDEPENDENT oracle (j5d_interp.c, same predicate table).
;
; THE LOAD-BEARING PART — IEEE UNORDERED (NaN): the ORDERED predicates (FBOGT/FBOR/…) are FALSE
; when an operand is NaN; the UNORDERED predicates (FBUN/FBULE/FBUGT/…) are TRUE on NaN. This
; program FTSTs a NaN and asserts the ordered-vs-unordered predicates take the OPPOSITE path.
;
;   a0 = RESULT scratch area (the test seeds a0 with a sandbox data address); FScc stores land
;        here, and the program builds an integer signature in d0 from the paths taken.
;   the vector table @ 0x00240000 is seeded by the test for FTRAPcc (vector 7) — handlers planted
;        by this program at entry (lea handler(pc) ; move.l into the table).

; ---- entry --------------------------------------------------------------------------------
    lea     fpconst,a1          ; a1 -> FP constants (relocated DATA)
    lea     result,a0           ; a0 -> RESULT scratch (relocated DATA)
    moveq   #0,d0               ; d0 = running signature

    ; install the FTRAPcc handler (vector 7) into the sandbox vector table @ 0x240000+7*4
    lea     trap7,a2
    move.l  a2,d1
    move.l  #$0024001C,a3       ; 0x240000 + 7*4 = 0x24001C
    move.l  d1,(a3)

    ; ================= PART 1: ORDERED comparison, no NaN ==================================
    ; fp0 = 3.0, fp1 = 5.0 ; fcmp fp1,fp0 -> compares fp0 ? fp1 = 3.0 ? 5.0 -> N=1 (less)
    fmove.d (a1),fp0           ; fp0 = 3.0
    fmove.d 8(a1),fp1          ; fp1 = 5.0
    fcmp.x  fp1,fp0            ; FPSR: 3.0 < 5.0 -> N=1 Z=0 NAN=0
    fblt    p1_lt              ; OLT true (N && !NAN && !Z) -> branch TAKEN
    bra     lfail               ; (should not reach)
p1_lt:
    addq.l  #1,d0              ; d0 += 1  (FBLT took the right path)
    fbgt    lfail               ; OGT false (3<5) -> NOT taken (fall through)
    addq.l  #2,d0              ; d0 += 2  (FBGT correctly fell through)
    fbeq    lfail               ; EQ false -> NOT taken
    fbne    p1_ne              ; NE true (Z==0) -> branch TAKEN
    bra     lfail
p1_ne:
    addq.l  #4,d0              ; d0 += 4

    ; ================= PART 2: EQUAL comparison ===========================================
    fmove.d (a1),fp2           ; fp2 = 3.0
    fcmp.x  fp2,fp0            ; 3.0 ? 3.0 -> Z=1 N=0 NAN=0
    fbeq    p2_eq              ; EQ true -> TAKEN
    bra     lfail
p2_eq:
    addq.l  #8,d0              ; d0 += 8
    fboge   p2_oge             ; OGE true (Z || (!N && !NAN)) -> TAKEN
    bra     lfail
p2_oge:
    add.l   #16,d0             ; d0 += 16

    ; ================= PART 3: THE NaN / UNORDERED CASE ===================================
    ; fp3 = NaN (from the constant pool). ftst fp3 -> NAN=1, N=0, Z=0.
    fmove.d 16(a1),fp3         ; fp3 = NaN
    ftst.x  fp3               ; FPSR: NAN=1 (unordered), N=0 Z=0
    fbor    lfail              ; OR (ordered) FALSE on NaN -> must NOT branch
    add.l   #32,d0            ; d0 += 32  (FBOR correctly fell through on NaN)
    fbun    p3_un             ; UN (unordered) TRUE on NaN -> must branch
    bra     lfail
p3_un:
    add.l   #64,d0            ; d0 += 64  (FBUN correctly branched on NaN)
    ; ordered GT is FALSE on NaN; the matching UNORDERED form (UGT) is TRUE on NaN
    fbogt   lfail              ; OGT false on NaN -> NOT taken
    add.l   #128,d0           ; d0 += 128
    fbugt   p3_ugt            ; UGT true on NaN -> TAKEN
    bra     lfail
p3_ugt:
    add.l   #256,d0           ; d0 += 256
    fbule   p3_ule            ; ULE true on NaN -> TAKEN
    bra     lfail
p3_ule:
    add.l   #512,d0           ; d0 += 512

    ; ================= PART 4: FScc (set byte on condition) ===============================
    ; reuse the NaN FPSR (still NAN=1 from the ftst fp3 above is stale — recompute cleanly):
    fmove.d (a1),fp4          ; fp4 = 3.0
    fmove.d 8(a1),fp5         ; fp5 = 5.0
    fcmp.x  fp5,fp4           ; 3.0 ? 5.0 -> N=1 (less)
    fslt    d3               ; OLT true -> d3.b = 0xFF
    fsgt    d4               ; OGT false -> d4.b = 0x00
    fseq    (a0)            ; EQ false -> [result+0].b = 0x00
    ftst.x  fp3              ; FPSR: NAN=1 (fp3 still NaN)
    fsun    1(a0)           ; UN true on NaN -> [result+1].b = 0xFF
    fsor    2(a0)           ; OR false on NaN -> [result+2].b = 0x00
    ; fold the FScc register results into d0: d3 low byte (0xFF) and d4 (0x00)
    move.b  d3,3(a0)         ; store d3.b (0xFF)
    move.b  d4,4(a0)         ; store d4.b (0x00)

    ; ================= PART 5: FDBcc (FP-condition decrement loop) ========================
    ; loop: count d5 from 3 down; FDBNE: while FP-NE is FALSE, decrement+branch. We set up an
    ; EQUAL FPSR (so FP-NE is FALSE) so the loop runs purely on the counter (3,2,1,0,-1 exit).
    fmove.d (a1),fp6          ; fp6 = 3.0
    fcmp.x  fp6,fp6           ; 3.0 ? 3.0 -> Z=1 (equal) ; so FP-NE is FALSE
    moveq   #3,d5             ; loop counter (word)
    moveq   #0,d6             ; iteration accumulator
fdb_loop:
    addq.l  #1,d6             ; count an iteration
    fdbne   d5,fdb_loop       ; FP-NE false -> decrement d5.W, branch while != -1
    ; condition never true -> body runs for d5 = 3,2,1,0 (4 iterations), then d5.W -> -1 exits.
    ; d6 = 4 ; d5.W = 0xFFFF (low word -1, high word preserved 0) -> d5 = 0x0000FFFF.
    move.l  d6,8(a0)          ; store the iteration count (big-endian .l) = 4

    ; ================= PART 6: FTRAPcc (trap on TRUE condition -> vector 7) ===============
    fcmp.x  fp6,fp6           ; 3.0 ? 3.0 -> Z=1 (equal)
    ftrapeq                   ; EQ true -> raise vector 7 ; handler sets d7 then returns
    ; (the handler at trap7 sets a marker and rte's back here)
    or.l    d7,d0             ; fold the handler's marker bit into d0

    bra     done

lfail:
    moveq   #-1,d0            ; FAIL signature (the test asserts d0 != -1 AND the byte-exact)
    bra     done

; ---- the FTRAPcc handler (vector 7). Sets d7 = a marker bit, then rte. ---------------------
trap7:
    move.l  #$00010000,d7     ; marker bit (folded into d0 after the trap returns)
    rte

done:
    rts

; ---- FP constant pool (relocated DATA: a1 = lea fpconst). All doubles (.d, IEEE binary64). ----
    cnop    0,8
fpconst:
    dc.l    $40080000,$00000000 ; +0  : 3.0
    dc.l    $40140000,$00000000 ; +8  : 5.0
    dc.l    $7FF80000,$00000000 ; +16 : NaN (quiet)

; ---- RESULT scratch area (relocated DATA: a0 = lea result). 64 bytes, zero-filled. -----------
    cnop    0,8
result:
    ds.b    64
