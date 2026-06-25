; j5l.s — movem (move-multiple-registers): the opcode every compiler-generated 68k
; function uses in its prologue/epilogue. Assembled with vasmm68k_mot -Fhunkexe to a
; REAL big-endian AmigaOS hunk executable, run through the [J5d..J5k] JIT engine
; (driving Emu68's REAL EMIT_MOVEM decoder, M68k_LINE4.c) byte-exact vs the independent
; interpreter.
;
; WHAT IT PROVES (the forms real compilers emit):
;   * movem.l d2-d7/a2-a6,-(sp)   PREDECREMENT save  (the PROLOGUE; REVERSED mask order)
;   * movem.l (sp)+,d2-d7/a2-a6   POSTINCREMENT restore (the EPILOGUE; normal order, An+=)
;   * movem.l <list>,(An)         control store/load to a fixed frame
;   * movem.l <list>,(d16,An)     displacement-mode store/load
;   * movem.w <list>,(An)         the .w forms (store low 16 bits / load sign-extended)
; called so the save/restore ACTUALLY MATTERS: a non-leaf subroutine `work` saves the
; callee-saved set, CLOBBERS every one of them in its body, restores them, and returns —
; and the caller asserts its d2-d7/a2-a6 survived. The byte-exact check (registers +
; sandbox memory, incl. the saved frame on the stack) is the real proof.
;
; CALLING CONVENTION (AmigaOS): d0/d1/a0/a1 are scratch (caller-saved); d2-d7/a2-a6 are
; callee-saved — exactly the set a compiler's movem prologue preserves.
;
; ----- a scratch frame in free sandbox space (above code, below the lib/heap region) -----
SCRATCH     equ     $00220000           ; 64 bytes of free sandbox RAM for the control frames

; Sentinels for the caller's callee-saved registers (must SURVIVE the call to `work`).
S2          equ     $11112222
S3          equ     $33334444
S4          equ     $55556666
S5          equ     $77778888
S6          equ     $9999AAAA
S7          equ     $BBBBCCCC
SA2         equ     $00221000           ; a2..a6 sentinels: in-sandbox addresses (so the
SA3         equ     $00221100           ;   restore is provably the saved value, and an An
SA4         equ     $00221200           ;   that leaks as an address would still be sane)
SA5         equ     $00221300
SA6         equ     $00221400

;==============================================================================
; main
;==============================================================================
    ; ---- seed the callee-saved registers the call must preserve ----
    move.l  #S2,d2
    move.l  #S3,d3
    move.l  #S4,d4
    move.l  #S5,d5
    move.l  #S6,d6
    move.l  #S7,d7
    move.l  #SA2,a2
    move.l  #SA3,a3
    move.l  #SA4,a4
    move.l  #SA5,a5
    move.l  #SA6,a6

    bsr     work                ; non-leaf subroutine: movem save/clobber/restore

    ; ---- assert every callee-saved register SURVIVED (==sentinel). d0 COUNTS the survivors
    ;      with addq.l #1 (in-subset, no immediate-source OR needed); a perfect run leaves
    ;      d0 = 11 ($0B) — d2-d7 (6) + a2-a6 (5). A leaked register fails to bump the count. ----
    moveq   #0,d0               ; survivor count
    move.l  #S2,d1
    cmp.l   d1,d2
    bne.s   .n2
    addq.l  #1,d0
.n2:
    move.l  #S3,d1
    cmp.l   d1,d3
    bne.s   .n3
    addq.l  #1,d0
.n3:
    move.l  #S4,d1
    cmp.l   d1,d4
    bne.s   .n4
    addq.l  #1,d0
.n4:
    move.l  #S5,d1
    cmp.l   d1,d5
    bne.s   .n5
    addq.l  #1,d0
.n5:
    move.l  #S6,d1
    cmp.l   d1,d6
    bne.s   .n6
    addq.l  #1,d0
.n6:
    move.l  #S7,d1
    cmp.l   d1,d7
    bne.s   .n7
    addq.l  #1,d0
.n7:
    ; address registers: move An -> d1, then cmp.l #imm,d1 (both in-subset; avoids cmpa).
    move.l  a2,d1
    cmp.l   #SA2,d1
    bne.s   .na2
    addq.l  #1,d0
.na2:
    move.l  a3,d1
    cmp.l   #SA3,d1
    bne.s   .na3
    addq.l  #1,d0
.na3:
    move.l  a4,d1
    cmp.l   #SA4,d1
    bne.s   .na4
    addq.l  #1,d0
.na4:
    move.l  a5,d1
    cmp.l   #SA5,d1
    bne.s   .na5
    addq.l  #1,d0
.na5:
    move.l  a6,d1
    cmp.l   #SA6,d1
    bne.s   .na6
    addq.l  #1,d0
.na6:
    rts                         ; TOP-LEVEL rts -> exit, d0 = survivor count (11/$0B on success)

;==============================================================================
; work — a non-leaf subroutine that does the compiler-style movem prologue/epilogue
;        around a body that CLOBBERS every callee-saved register. Also exercises the
;        control-mode + (d16,An) + .w movem forms against a fixed sandbox frame.
;==============================================================================
work:
    movem.l d2-d7/a2-a6,-(sp)   ; PROLOGUE: predecrement save (reversed mask order)

    ; ---- body: clobber every saved register with garbage ----
    moveq   #-1,d2
    moveq   #-1,d3
    moveq   #-1,d4
    moveq   #-1,d5
    moveq   #-1,d6
    moveq   #-1,d7
    move.l  #$DEAD0002,a2
    move.l  #$DEAD0003,a3
    move.l  #$DEAD0004,a4
    move.l  #$DEAD0005,a5
    move.l  #$DEAD0006,a6

    ; ---- exercise the CONTROL-MODE + displacement + .w movem forms on a fixed frame ----
    ; Seed d0-d3 with known values, round-trip them through (An), (d16,An) and .w stores,
    ; so the byte-exact memory check covers the control-store/load + half-width paths.
    move.l  #SCRATCH,a0         ; a0 -> the scratch frame base
    move.l  #$0A0B0C0D,d0
    move.l  #$1A1B1C1D,d1

    movem.l d0-d1,(a0)          ; control store (An): [a0]=d0, [a0+4]=d1   (.l)
    movem.l (a0),d4-d5          ; control load  (An): d4=d0, d5=d1
    ; d4,d5 now hold $0A0B0C0D,$1A1B1C1D (clobbered again below; we only need the mem image)

    movem.l d0-d1,32(a0)        ; (d16,An) store: [a0+32]=d0, [a0+36]=d1
    movem.l 32(a0),d6-d7        ; (d16,An) load:  d6=d0, d7=d1

    move.l  #$00007FFF,d0       ; .w store/load: positive word -> stays positive on load
    move.l  #$FFFF8001,d1       ; .w: low word 0x8001 -> sign-extends to 0xFFFF8001 on load
    movem.w d0-d1,48(a0)        ; .w store: [a0+48]=0x7FFF, [a0+50]=0x8001  (low 16 bits)
    movem.w 48(a0),d2-d3        ; .w load (SIGN-EXTENDED): d2=$00007FFF, d3=$FFFF8001

    movem.l (sp)+,d2-d7/a2-a6   ; EPILOGUE: postincrement restore (normal order, sp += 44)
    rts                         ; return to main; the caller's d2-d7/a2-a6 are restored
