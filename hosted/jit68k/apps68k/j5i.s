; j5i.s — [J5i] the 68k EXCEPTION / SR model exerciser. A REAL big-endian AmigaOS hunk
; executable (vasmm68k_mot -Fhunkexe -no-opt) that installs 68k exception handlers in the
; vector table, then deliberately raises three exceptions from three REAL causes and lets
; the handlers record them and rte-resume — proving the dispatcher builds the correct frame
; (SR + return PC on the supervisor stack), vectors to the right handler, and that rte pops
; the frame and resumes. The div-by-zero comes from a genuine divu.w #0; the trap from a
; genuine trap #1; the illegal from the genuine ILLEGAL opcode (0x4AFC).
;
; The vector table is at J5I_VBR = 0x00240000 (our fixed VBR stand-in; the real VBR register
; is deferred — see spec [J5i]). The program plants each handler's ABSOLUTE pc there with
;   lea handler(pc),a0 ; move.l a0, J5I_VBR + vec*4
; (PC-relative lea needs no relocation; move.l abs.l writes the slot).
;
; Vectors covered:
;   trap #1        -> vector 33 (= 32 + 1)   ; handler tallies bit, rte resumes after trap
;   divu.w #0,d0   -> vector 5  (div by zero); handler tallies bit, rte resumes after divu
;   ILLEGAL        -> vector 4               ; handler tallies bit, then JMP to finish (this
;                                              is the "handler records + HALTS" path — it does
;                                              not rte back to the faulting word, so no loop)
;
; d7 accumulates a bitmask of which handlers ran:  bit0=trap, bit1=div0, bit2=illegal.
; The final exit code packs proof of all three plus a marker:
;   d0 = (d7 << 8) | $5A      ; d7 should be %111 = 7  ->  d0 = 0x0000075A
; A wrong/missing exception (handler not reached, or rte resumed at the wrong PC) changes d7
; and the exit code diverges from the oracle.

VBR     equ     $00240000          ; the [J5i] vector-table base (J5I_VBR)

;------------------------------------------------------------------- entry
        moveq   #0,d7              ; d7 = exception tally (starts empty)

        ; --- install the three handlers into the vector table ---
        lea     trap1_h(pc),a0
        move.l  a0,VBR+33*4        ; vector 33  = trap #1

        lea     div0_h(pc),a0
        move.l  a0,VBR+5*4         ; vector 5   = divide-by-zero

        lea     ill_h(pc),a0
        move.l  a0,VBR+4*4         ; vector 4   = illegal instruction

        ; --- cause #1: TRAP #1 (group 2; frame PC = the instruction AFTER) ---
        trap    #1                 ; -> vector 33 ; handler sets bit0, rte returns here+2

        ; --- cause #2: divide by zero (divu.w #0,d0 -> vector 5) ---
        moveq   #10,d0             ; a nonzero dividend so the result, if it ran, is visible
        divu.w  #0,d0              ; ZERO divisor -> vector 5 ; rte returns to here+4

        ; --- cause #3: ILLEGAL instruction (0x4AFC -> vector 4) ---
        ; The illegal handler does NOT rte (the frame PC is the faulting word; an rte would
        ; re-execute it forever). It tallies bit2 then jumps to `finish` — the "handler
        ; records + halts/redirects" path. Control never falls through this illegal.
        illegal                   ; -> vector 4 ; handler tallies + jmp finish

        ; (unreached: the illegal handler redirects to `finish`)

finish:
        ; --- all three exceptions handled; pack the proof into d0 ---
        move.l  d7,d0              ; d0 = tally (expect %111 = 7)
        lsl.l   #8,d0              ; d0 = tally << 8
        ori.l   #$5A,d0            ; d0 = (tally<<8) | 0x5A  -> 0x0000075A
        rts                        ; top-level rts -> exit, d0 = 0x0000075A

;------------------------------------------------------------------- handlers
; Each handler runs in supervisor state (the dispatcher set S on entry). They touch only
; d7 (the tally) so they do not perturb the program's other registers, then rte.

trap1_h:
        ori.l   #1,d7              ; bit0 : trap #1 handled
        rte                        ; pop SR+PC, resume after the trap

div0_h:
        ori.l   #2,d7              ; bit1 : div-by-zero handled
        rte                        ; resume after the divu.w (frame PC = instr-after)

; The illegal frame's saved PC is the FAULTING instruction itself (group-1 convention). A
; plain rte would re-execute it forever, so this handler records and JUMPS to `finish`
; instead of returning (the "handler records + halts/redirects" path). The frame it left on
; the stack is harmless — finish does not touch it (top-level rts pops the original return).
ill_h:
        ori.l   #4,d7              ; bit2 : illegal handled
        addq.l  #6,a7              ; pop our own 6-byte exception frame (we do not rte)
        jmp     finish             ; redirect (no rte) -> reach the exit-pack sequence
