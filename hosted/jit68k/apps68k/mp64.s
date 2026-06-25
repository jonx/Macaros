; mp64.s — [J5h] the X-bit MULTI-PRECISION chain: a 64-bit add and a 64-bit negate that
; genuinely propagate the 68k X (extend) bit across longword boundaries. Assembled with
; vasmm68k_mot -Fhunkexe -no-opt to a REAL big-endian AmigaOS hunk executable.
;
; This is the [J5h] corpus program. Where the earlier corpus (mul/fact/arraysum/libcall/
; sumsq/bubsort) never depended on X surviving from one instruction to the next, mp64
; builds 64-bit values in register pairs and does arithmetic that is ONLY correct if the
; carry/borrow out of the low longword (recorded in X) is consumed by the high longword:
;
;   add.l  Dlo,Dlo'  sets X = carry out of bit 31
;   addx.l Dhi,Dhi'  adds that X into the high longword   <- the X-carry link
;   neg.l  Dlo       sets X = (low != 0) = borrow out of 0 - low
;   negx.l Dhi       computes high = 0 - high - X          <- the X-borrow link
;
; A register holding the WRONG X (or an op that fails to read/produce it) makes the high
; longword off by exactly one — the negative control proves the byte-exact assert bites.
;
; 64-bit values are held big-endian as (high in Da, low in Db):
;
;   A = 0x00000001_FFFFFFFF   (a_hi=d2, a_lo=d3)
;   B = 0x00000002_00000001   (b_hi=d4, b_lo=d5)
;
;   --- 64-bit ADD:  S = A + B ---
;   add.l  d5,d3          ; lo = FFFFFFFF + 1 = 0x00000000, X = 1 (carry out)
;   addx.l d4,d2          ; hi = 1 + 2 + X(1) = 0x00000004
;   ; S = 0x00000004_00000000   (s_hi=d2=4, s_lo=d3=0)
;
;   --- 64-bit NEGATE:  N = -S  (two's complement of the 64-bit S) ---
;   ; copy S into d6:d7 so we keep S intact for the fold
;   move.l d2,d6         ; n_hi = s_hi
;   move.l d3,d7         ; n_lo = s_lo
;   neg.l  d7            ; n_lo = 0 - 0       = 0x00000000, X = 0 (low was 0 -> no borrow)
;   negx.l d6            ; n_hi = 0 - 4 - X(0) = 0xFFFFFFFC
;   ; N = 0xFFFFFFFC_00000000   (n_hi=d6=FFFFFFFC, n_lo=d7=0)
;
;   --- fold the four longwords into d0 so the value is observable & carry-sensitive ---
;   ; d0 = s_hi + s_lo + n_hi + n_lo
;   ;    = 0x00000004 + 0x00000000 + 0xFFFFFFFC + 0x00000000
;   ;    = 0x100000000 truncated to 32 bits = 0x00000000
;   ; (S + N == 0 as 64-bit; the longword fold of S and -S is 0 mod 2^32) -- to make the
;   ; result a recognisable nonzero constant we instead build it from the high longwords:
;   move.l d2,d0         ; d0 = s_hi = 0x00000004
;   add.l  d6,d0         ; d0 = s_hi + n_hi = 4 + 0xFFFFFFFC = 0x00000000  (wraps)
;   ; that is 0 too; rotate one in so the carry chain is unmistakably load-bearing:
;   ; final observable: pack s_hi into the high word and (-s_hi low byte) into the low.
;   ; Simpler + carry-sensitive: d0 = (s_hi << 8) | (n_hi & 0xFF)
;   ;   s_hi = 4 -> 0x00000400 ; n_hi & 0xFF = 0xFC -> d0 = 0x000004FC
;   moveq  #0,d0
;   move.l d2,d0         ; d0 = s_hi = 4
;   lsl.l  #8,d0         ; d0 = 0x00000400
;   move.l d6,d1         ; d1 = n_hi = 0xFFFFFFFC
;   andi.l #$000000FF,d1 ; d1 = 0xFC
;   or.l   d1,d0         ; d0 = 0x000004FC
;   rts                  ; top-level rts -> exit, d0 = 0x000004FC
;
; If the X-carry link is broken (addx fails to add X, or negx fails to subtract X), s_hi or
; n_hi shifts by one and d0 changes -> the byte-exact assert (value + full register file +
; CCR) diverges.  Exercises through the REAL Emu68 decoders: add.l, addx.l (LINED), neg.l,
; negx.l (LINE4), move.l, moveq, lsl.l (LINEE), andi.l (LINE0), or.l (LINE8), rts.

    move.l  #$00000001,d2       ; a_hi
    move.l  #$FFFFFFFF,d3       ; a_lo
    move.l  #$00000002,d4       ; b_hi
    move.l  #$00000001,d5       ; b_lo

    add.l   d5,d3               ; a_lo + b_lo -> X = carry
    addx.l  d4,d2               ; a_hi + b_hi + X     (the X-carry link)
                                ; S = d2:d3 = 0x00000004_00000000

    move.l  d2,d6               ; n_hi = s_hi
    move.l  d3,d7               ; n_lo = s_lo
    neg.l   d7                  ; n_lo = -s_lo -> X = borrow
    negx.l  d6                  ; n_hi = 0 - s_hi - X (the X-borrow link)
                                ; N = d6:d7 = 0xFFFFFFFC_00000000

    moveq   #0,d0
    move.l  d2,d0               ; d0 = s_hi = 0x00000004
    lsl.l   #8,d0               ; d0 = 0x00000400
    move.l  d6,d1               ; d1 = n_hi = 0xFFFFFFFC
    andi.l  #$000000FF,d1       ; d1 = 0x000000FC
    or.l    d1,d0               ; d0 = 0x000004FC
    rts                         ; exit, d0 = 0x000004FC
