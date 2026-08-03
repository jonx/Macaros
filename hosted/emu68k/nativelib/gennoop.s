; gennoop.s - source-proven no-op crossing with deliberately invalid pointers.
;
; Native AROS documents and implements GT_RefreshWindow as an empty
; compatibility vector. The generated bridge must therefore return without
; translating or dereferencing either pointer argument.

EXEC_OpenLibrary equ -552
DOS_PutStr        equ -948
GT_RefreshWindow equ -84

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     gadtoolsname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed

    move.l  d0,a6
    moveq   #1,d0
    move.l  d0,a0
    moveq   #2,d0
    move.l  d0,a1
    jsr     GT_RefreshWindow(a6)

    lea     passmsg(pc),a0
    bra.s   print
failed:
    lea     failmsg(pc),a0
print:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:      dc.b "dos.library",0
gadtoolsname: dc.b "gadtools.library",0
passmsg:      dc.b "[T3NOOP] PASS",10,0
failmsg:      dc.b "[T3NOOP] FAIL",10,0
    even
