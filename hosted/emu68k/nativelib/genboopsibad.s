; genboopsibad.s - reject an invalid guest IClass dispatcher before NewObjectA.

EXEC_OpenLibrary equ -552
DOS_PutStr       equ -948
INT_NewObjectA   equ -636
INT_MakeClass    equ -678
ICLASS_h_Entry   equ 8

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5
    move.l  4.w,a6
    lea     intuitionname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,a6
    suba.l  a0,a0
    lea     rootclass(pc),a1
    suba.l  a2,a2
    moveq   #0,d0
    moveq   #0,d1
    jsr     INT_MakeClass(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,a0
    clr.l   ICLASS_h_Entry(a0)
    suba.l  a1,a1
    suba.l  a2,a2
    jsr     INT_NewObjectA(a6)
failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:       dc.b "dos.library",0
intuitionname: dc.b "intuition.library",0
rootclass:     dc.b "rootclass",0
failmsg: dc.b "[T3BOOPSI-BAD] FAIL: invalid dispatcher reached native code",10,0
