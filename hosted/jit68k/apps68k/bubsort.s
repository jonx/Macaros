; bubsort.s — the [J5g] DEMANDING program: a bubble sort of an array of longwords
; (indexed memory addressing) followed by a checksum/mixer over the sorted array using
; the FULL shift/rotate set + immediate (LINE0) + misc (LINE4) opcodes. Assembled with
; vasmm68k_mot -Fhunkexe -no-opt to a REAL big-endian AmigaOS hunk executable WITH a
; HUNK_DATA section + a HUNK_RELOC32 fixup (lea arr,a0).
;
; -no-opt is REQUIRED: it keeps the exact opcodes written (vasm would otherwise rewrite
; addi #n -> addq, lea d16(An),An -> addq An, etc., collapsing the very coverage we want
; to exercise). Target CPU = plain M68000 (NO 68020 scale factors / no PMMU).
;
; This program substantially BROADENS the ISA + addressing-mode coverage the JIT drives
; through Emu68's REAL per-opcode decoders, beyond the [J5d]..[J5f] register/ALU/control
; set. EVERY opcode + addressing mode + size is INDEPENDENTLY checked, byte-exact, by the
; from-scratch interpreter oracle (j5d_interp.c) — nothing runs that the oracle can't verify.
;
; COVERAGE (all via the REAL Emu68 decoders unless noted "dispatcher"):
;   LINE0 imm:   addi.l ; subi.l ; andi.l ; ori.l ; eori.l ; cmpi.l ; btst #b,Dn
;   LINE4 misc:  clr.l ; tst.l ; swap ; ext.l   (neg.l/not.l deferred — X-bit chain)
;   LINEE shift: lsl.l #c,Dn ; lsr.l #c,Dn ; asl.l #c,Dn ; asr.l #c,Dn ; rol.l #c,Dn ; ror.l #c,Dn
;   addr modes:  (d8,An,Xn.L)  M68000 indexed (array element LOAD and STORE)
;                (d16,An)      displacement  (lea/move via 4(a1))
;                (An)          register indirect
;                abs.l         (lea, relocated; dispatcher)
;                #imm          immediate (LINE0)
;   size:        .L
;   control:     bsr/rts subroutine, cmp.l/ble.s/blt.s/bne.s loops, addq/subq, bra.s
;
;   The array (6 longwords, unsorted):  17, 3, 42, 8, 99, 23
;   After an ascending bubble sort:      3, 8, 17, 23, 42, 99
;   The checksum/mixer folds the sorted array with the full shift/rotate/immediate set;
;   the exact value is computed identically by the oracle. Returned in d0.

        machine 68000

N       equ     6

;==============================================================================
; main: sort the array in place, then mix it into a checksum.
; d2 / d3 are BYTE offsets (element index * 4) so indexed (0,a0,Dn.L) addresses
; longwords without a 68020 scale factor (plain M68000).
;==============================================================================
        lea     arr,a0           ; a0 = &array  (abs.l, relocated into the sandbox)

;----- bubble sort (ascending) ------------------------------------------------
        moveq   #N-1,d7          ; d7 = number of passes = N-1
outer:
        moveq   #0,d2            ; d2 = byte offset of a[j], j=0
        move.l  d7,d6            ; d6 = inner trip count
inner:
        move.l  (0,a0,d2.l),d4   ; d4 = a[j]      ((d8,An,Xn.L) indexed load)
        move.l  d2,d3
        addi.l  #4,d3            ; d3 = byte offset of a[j+1]   (LINE0 addi.l #imm)
        move.l  (0,a0,d3.l),d5   ; d5 = a[j+1]   (indexed load, index in d3)
        cmp.l   d5,d4            ; a[j] - a[j+1]
        ble.s   noswap           ; if a[j] <= a[j+1] keep order (signed)
        move.l  d5,(0,a0,d2.l)   ; a[j]   = old a[j+1]   (indexed STORE)
        move.l  d4,(0,a0,d3.l)   ; a[j+1] = old a[j]
noswap:
        addi.l  #4,d2            ; next j (byte offset += 4)
        subq.l  #1,d6            ; inner trip--
        bne.s   inner
        subq.l  #1,d7            ; pass--
        bne.s   outer            ; while passes != 0

;----- checksum/mixer --------------------------------------------------------
        bsr     mixer            ; d0 = mixer(a0)
        rts                      ; top-level rts -> program exit, d0 = checksum

;==============================================================================
; mixer subroutine: a0 -> array.  Returns the folded checksum in d0.
; Exercises (An), (d16,An), the FULL shift/rotate set, and the LINE0 immediates.
;==============================================================================
mixer:
        clr.l   d0               ; c = 0                 (LINE4 clr.l)
        move.l  a0,a1            ; a1 walks the array    (movea.l An,Am)
        moveq   #N,d6            ; loop count
mxloop:
        move.l  (a1),d4          ; v = *a1               ((An) load)
        add.l   d4,d0            ; c += v
        rol.l   #5,d0            ; LINEE rol.l #c,Dn
        eori.l  #$5A5A5A5A,d0    ; LINE0 eori.l #imm,Dn
        lea     4(a1),a1         ; a1 += 4   ((d16,An) -> LINE4 EMIT_LEA, EA mode 5)
        subq.l  #1,d6
        bne.s   mxloop

        ; ---- final fold: exercise the remaining shift/rotate/immediate/misc opcodes ----
        move.l  d0,d2            ; keep a copy
        swap    d2               ; LINE4 swap   (d2 = rotate halves)
        eor.l   d2,d0            ; LINEB eor.l  (fold)
        lsl.l   #3,d0            ; LINEE lsl.l #c,Dn
        lsr.l   #1,d0            ; LINEE lsr.l #c,Dn
        asl.l   #2,d0            ; LINEE asl.l #c,Dn
        asr.l   #1,d0            ; LINEE asr.l #c,Dn
        ror.l   #7,d0            ; LINEE ror.l #c,Dn
        ori.l   #$01010101,d0    ; LINE0 ori.l  #imm,Dn
        andi.l  #$00FFFFFF,d0    ; LINE0 andi.l #imm,Dn (mask to 24 bits)
        addi.l  #$1234,d0        ; LINE0 addi.l #imm,Dn
        subi.l  #$0034,d0        ; LINE0 subi.l #imm,Dn

        ; ---- a few flag/misc opcodes whose RESULT is observable in d3 ----
        ; (NOTE: neg.l/not.l are deliberately NOT used here — their X-bit behaviour in a
        ;  long block is the deferred "X-bit multi-precision chain" the JIT does not yet
        ;  reproduce byte-exact vs the textbook 68k X; this program only contains opcodes
        ;  whose FULL CCR — including X — the oracle verifies byte-exact. See spec [J5g].)
        move.w  d0,d3            ; low 16 bits of c into d3 (move.w sets d3 low word)
        ext.l   d3               ; LINE4 ext.l  (sign-extend word -> long)
        cmpi.l  #0,d3            ; LINE0 cmpi.l #imm,Dn  (sets flags; d3 unchanged)
        btst    #0,d0            ; LINE0 btst #bit,Dn    (Z = !(bit0 of c))
        tst.l   d0               ; LINE4 tst.l  (sets N/Z from c)
        rts                      ; return checksum in d0

;==============================================================================
        section data,data
arr:
        dc.l    17
        dc.l    3
        dc.l    42
        dc.l    8
        dc.l    99
        dc.l    23
