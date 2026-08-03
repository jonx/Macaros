; genrecordbad.s - negative control for terminated-record array policy.
;
; IM_ITEM makes nm_Label an Image pointer rather than a string. Until that
; object contract exists, the generic importer must stop at the boundary and
; report the rejected variant instead of guessing.

EXEC_OpenLibrary equ -552
DOS_PutStr        equ -948
GT_CreateMenusA   equ -48

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

    lea     imagemenu(pc),a0
    suba.l  a1,a1
    jsr     GT_CreateMenusA(a6)    ; must stop at the bridge policy check

failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:      dc.b "dos.library",0
gadtoolsname: dc.b "gadtools.library",0
failmsg:      dc.b "[T3RECORD-BAD] FAIL: image record reached native code",10,0
    even

imagemenu:
    dc.b $82,0                     ; IM_ITEM: nm_Label is not a C string
    dc.l 0,0
    dc.w 0
    dc.l 0,0
    dc.b 0,0
    dc.l 0,0
    dc.w 0
    dc.l 0,0
