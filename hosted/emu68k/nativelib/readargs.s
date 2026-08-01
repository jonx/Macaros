; ReadArgs test: template "FILE/A,COUNT/N,ALL/S" then print what came back
    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     -552(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5
    lea     tmpl(pc),a0
    move.l  a0,d1
    lea     args(pc),a0
    move.l  a0,d2
    moveq   #0,d3
    move.l  a5,a6
    jsr     -798(a6)          ; ReadArgs
    tst.l   d0
    beq.s   failed
    ; print FILE (args[0])
    lea     args(pc),a0
    move.l  (a0),d2           ; the string pointer ReadArgs produced
    beq.s   noargs
    move.l  a5,a6
    jsr     -60(a6)           ; Output
    move.l  d0,d1
    moveq   #20,d3            ; print up to 20 bytes of it
    move.l  a5,a6
    jsr     -48(a6)           ; Write
    moveq   #0,d0
    rts
noargs:
    moveq   #1,d0
    rts
failed:
    moveq   #2,d0
    rts
done:
    moveq   #3,d0
    rts
dosname: dc.b "dos.library",0
    cnop 0,2
tmpl:    dc.b "FILE/A,COUNT/N,ALL/S",0
    cnop 0,4
args:    dc.l 0,0,0
