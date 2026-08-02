; gentagbad.s - negative control for the generated TagItem type compiler.
;
; An unknown tag must stop as a named capability gap. If it reaches native
; graphics.library, or the generated crossing guesses that it is scalar, this
; program returns and prints FAIL.

EXEC_OpenLibrary  equ -552
DOS_PutStr        equ -948
GFX_BestModeIDA   equ -1050

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     gfxname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed

    move.l  d0,a6
    lea     badtags(pc),a0
    jsr     GFX_BestModeIDA(a6)     ; must terminate at the bridge gap

failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname: dc.b "dos.library",0
gfxname: dc.b "graphics.library",0
failmsg: dc.b "[T3TAG] FAIL: unknown tag was guessed",10,0
    even
badtags:
    dc.l $8fffffff,$12345678
    dc.l 0,0
