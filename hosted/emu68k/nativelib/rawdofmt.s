; rawdofmt.s - exec.library RawDoFmt, implemented as 68k code that runs IN THE
; GUEST.
;
; RawDoFmt is the formatting engine every Amiga program's printf funnels into,
; and it takes a CALLBACK: PutChProc points at the caller's own 68k code and is
; invoked once per character. That is what makes it the wrong shape for the
; native bridge. Bridging it would mean native AROS code calling back into the
; guest for every character, which needs the JIT re-entered mid-call; and the
; argument block cannot be converted without parsing the format string first,
; because a %s argument is a guest pointer while a %d argument is not.
;
; Running the whole thing in the guest makes all of that disappear. Format
; string, argument block, %s pointers and PutChProc are all guest addresses in
; one address space, and the callback is an ordinary `jsr (a2)`.
;
; The bridge does not execute this - it REDIRECTS to it, so the guest runs it as
; though the library vector had been a normal subroutine.
;
;   RawDoFmt(FormatString A0, DataStream A1, PutChProc A2, PutChData A3)
;   -> D0 = the next unread data item
;
; PutChProc is called with the character in D0 and the data pointer in A3, and
; may clobber D0/D1/A0/A1 only - which is why every piece of state below lives
; in D2-D7/A4-A6. A3 is deliberately NOT protected across the call: the
; universal sprintf-style callback is `move.b d0,(a3)+ ; rts`, so the callee
; advancing it is the mechanism, not a bug.
;
; Supported: %[-][0][width][.limit][l]{d,u,x,X,c,s,b} and %%. Integers are
; 16-bit unless `l` is given, per the AmigaOS contract.
;
; Registers:  a2 PutChProc   a3 PutChData (live)   a4 format   a5 data
;             a6 scratch pointer (string / pow10 cursor)
;             d2 flags (bit0 left, bit1 zero-pad, bit2 negative)
;             d3 width   d4 .limit (-1 none)   d5 item length   d6 value
;             d7 digit cursor

RawDoFmt:
        movem.l d2-d7/a2-a6,-(sp)
        movea.l a0,a4
        movea.l a1,a5

.next:
        moveq   #0,d0
        move.b  (a4)+,d0
        beq     .done
        cmp.b   #'%',d0
        beq.s   .spec
        bsr     .putch
        bra.s   .next

; ---- parse a directive -----------------------------------------------------
.spec:
        moveq   #0,d2
        moveq   #0,d3
        moveq   #-1,d4
        moveq   #0,d5                   ; d5 doubles as the size flag until the
                                        ; argument is fetched, then as the
                                        ; item length for padding
.flags:
        move.b  (a4),d0
        cmp.b   #'-',d0
        bne.s   .fl0
        bset    #0,d2
        addq.l  #1,a4
        bra.s   .flags
.fl0:
        cmp.b   #'0',d0
        bne.s   .width
        bset    #1,d2
        addq.l  #1,a4
        bra.s   .flags

.width:
        moveq   #0,d0
        move.b  (a4),d0
        cmp.b   #'0',d0
        blt.s   .dot
        cmp.b   #'9',d0
        bgt.s   .dot
        addq.l  #1,a4
        sub.l   #'0',d0
        mulu    #10,d3
        add.l   d0,d3
        bra.s   .width

.dot:
        cmp.b   #'.',(a4)
        bne.s   .size
        addq.l  #1,a4
        moveq   #0,d4
.lim:
        moveq   #0,d0
        move.b  (a4),d0
        cmp.b   #'0',d0
        blt.s   .size
        cmp.b   #'9',d0
        bgt.s   .size
        addq.l  #1,a4
        sub.l   #'0',d0
        mulu    #10,d4
        add.l   d0,d4
        bra.s   .lim

.size:
        cmp.b   #'l',(a4)
        bne.s   .type
        addq.l  #1,a4
        bset    #3,d2                   ; bit3 = 32-bit argument

.type:
        moveq   #0,d0
        move.b  (a4)+,d0
        beq     .done                   ; a trailing '%': stop cleanly
        cmp.b   #'c',d0
        beq     .fmt_c
        cmp.b   #'s',d0
        beq     .fmt_s
        cmp.b   #'b',d0
        beq     .fmt_b
        cmp.b   #'d',d0
        beq     .fmt_d
        cmp.b   #'u',d0
        beq     .fmt_u
        cmp.b   #'x',d0
        beq     .fmt_x
        cmp.b   #'X',d0
        beq     .fmt_x
        ; '%%' and anything unrecognised are emitted literally, so a stray '%'
        ; costs one character rather than the rest of the line
        bsr     .putch
        bra     .next

.fmt_c:
        bsr     .fetch
        move.l  d6,d0
        bsr     .putch
        bra     .next

; ---- %s and %b -------------------------------------------------------------
.fmt_s:
        move.l  (a5)+,d6                ; a string argument is always a pointer
        movea.l d6,a6
        moveq   #0,d5
.ss_len:
        tst.b   (a6)+
        beq.s   .ss_have
        addq.l  #1,d5
        bra.s   .ss_len
.ss_have:
        movea.l d6,a6
        bra.s   .str_go

.fmt_b:                                 ; BSTR: a BPTR, so <<2, length byte first
        move.l  (a5)+,d6
        lsl.l   #2,d6
        movea.l d6,a6
        moveq   #0,d5
        move.b  (a6)+,d5

.str_go:
        tst.l   d4                      ; a .limit truncates
        bmi.s   .str_pad
        cmp.l   d4,d5
        ble.s   .str_pad
        move.l  d4,d5
.str_pad:
        bsr     .padlead
.str_emit:
        tst.l   d5
        beq.s   .str_tail
        moveq   #0,d0
        move.b  (a6)+,d0
        bsr     .putch
        subq.l  #1,d5
        bra.s   .str_emit
.str_tail:
        move.l  d3,d5                   ; everything is emitted; padtail wants a
        bsr     .padtail                ; length, and the emitted count is gone
        bra     .next

; ---- integers --------------------------------------------------------------
.fmt_d:
        bsr     .fetch
        btst    #3,d2
        bne.s   .d32
        ext.l   d6                      ; a 16-bit %d is signed
.d32:
        tst.l   d6
        bpl.s   .d_pos
        neg.l   d6
        bset    #2,d2                   ; remember the minus sign
.d_pos:
        bsr     .num_dec
        bra     .next

.fmt_u:
        bsr     .fetch
        bsr     .num_dec
        bra     .next

.fmt_x:
        bsr     .fetch
        bsr     .num_hex
        bra     .next

; ---- helpers ---------------------------------------------------------------
; .fetch: the next argument into d6. A 16-bit argument occupies a WORD of the
; stream, so the cursor advances by 2 rather than 4.
.fetch:
        btst    #3,d2
        bne.s   .fe_long
        moveq   #0,d6
        move.w  (a5)+,d6
        rts
.fe_long:
        move.l  (a5)+,d6
        rts

; .num_dec: d6 = magnitude, bit2 of d2 = minus wanted.
.num_dec:
        bsr     .dec_prep               ; a6 -> first significant power, d7 = n-1
        move.l  d7,d5
        addq.l  #1,d5                   ; d5 = digit count
        btst    #2,d2
        beq.s   .nd_pad
        addq.l  #1,d5                   ; the sign occupies a column too
.nd_pad:
        btst    #1,d2
        beq.s   .nd_space
        btst    #2,d2                   ; zero padding: the sign comes FIRST,
        beq.s   .nd_lead                ; otherwise it lands after the zeros
        moveq   #'-',d0
        bsr     .putch
        bclr    #2,d2
.nd_lead:
        bsr     .padlead
        bra.s   .nd_digits
.nd_space:
        bsr     .padlead
        btst    #2,d2
        beq.s   .nd_digits
        moveq   #'-',d0
        bsr     .putch
.nd_digits:
        bsr     .dec_out
        bsr     .padtail
        rts

; .num_hex: d6 = value.
.num_hex:
        bsr     .hex_prep               ; d7 = shift of the top nibble
        move.l  d7,d5
        lsr.l   #2,d5
        addq.l  #1,d5                   ; d5 = nibble count
        bsr     .padlead
        bsr     .hex_out
        bsr     .padtail
        rts

; .dec_prep: a6 -> the first power of ten that fits in d6, d7 = digits - 1.
.dec_prep:
        lea     .pow10(pc),a6
        moveq   #9,d7
.dp_loop:
        tst.l   d7
        beq.s   .dp_done                ; the last entry is 1: always significant
        move.l  (a6),d0
        cmp.l   d0,d6
        bcc.s   .dp_done                ; unsigned: d6 >= power
        addq.l  #4,a6
        subq.l  #1,d7
        bra.s   .dp_loop
.dp_done:
        rts

; .dec_out: emit the digits, most significant first. Division is repeated
; subtraction of a power of ten - at most nine per digit, and it needs no
; 32-bit divide instruction.
.dec_out:
.do_loop:
        move.l  (a6),d1
        moveq   #0,d0
.do_sub:
        cmp.l   d1,d6
        bcs.s   .do_have                ; unsigned: d6 < power
        sub.l   d1,d6
        addq.l  #1,d0
        bra.s   .do_sub
.do_have:
        add.l   #'0',d0
        bsr     .putch
        addq.l  #4,a6
        tst.l   d7
        beq.s   .do_done
        subq.l  #1,d7
        bra.s   .do_loop
.do_done:
        rts

; .hex_prep: d7 = the shift count of the highest non-zero nibble (0 if zero).
.hex_prep:
        moveq   #28,d7
.hp_loop:
        tst.l   d7
        beq.s   .hp_done
        move.l  d6,d0
        move.l  d7,d1
        lsr.l   d1,d0
        and.l   #15,d0
        bne.s   .hp_done
        subq.l  #4,d7
        bra.s   .hp_loop
.hp_done:
        rts

.hex_out:
.ho_loop:
        move.l  d6,d0
        move.l  d7,d1
        lsr.l   d1,d0
        and.l   #15,d0
        cmp.l   #10,d0
        blt.s   .ho_dig
        add.l   #'a'-10,d0
        bra.s   .ho_put
.ho_dig:
        add.l   #'0',d0
.ho_put:
        bsr     .putch
        subq.l  #4,d7
        bpl.s   .ho_loop
        rts

; .padlead / .padtail: d5 = the length of the item itself, d3 = the field width.
.padlead:
        btst    #0,d2                   ; left-aligned: the padding goes after
        bne.s   .pl_ret
        move.l  d3,d1
        sub.l   d5,d1
        ble.s   .pl_ret
.pl_loop:
        move.l  d1,-(sp)
        moveq   #' ',d0
        btst    #1,d2
        beq.s   .pl_emit
        moveq   #'0',d0
.pl_emit:
        bsr     .putch
        move.l  (sp)+,d1
        subq.l  #1,d1
        bgt.s   .pl_loop
.pl_ret:
        rts

.padtail:
        btst    #0,d2
        beq.s   .pt_ret
        move.l  d3,d1
        sub.l   d5,d1
        ble.s   .pt_ret
.pt_loop:
        move.l  d1,-(sp)
        moveq   #' ',d0
        bsr     .putch
        move.l  (sp)+,d1
        subq.l  #1,d1
        bgt.s   .pt_loop
.pt_ret:
        rts

; .putch: hand one character to the caller's routine.
.putch:
        and.l   #$ff,d0
        jsr     (a2)
        rts

.done:
        moveq   #0,d0                   ; RawDoFmt NUL-terminates its output
        bsr     .putch
        move.l  a5,d0                   ; -> the next unread data item
        movem.l (sp)+,d2-d7/a2-a6
        rts

        cnop    0,4
.pow10:
        dc.l    1000000000
        dc.l    100000000
        dc.l    10000000
        dc.l    1000000
        dc.l    100000
        dc.l    10000
        dc.l    1000
        dc.l    100
        dc.l    10
        dc.l    1
