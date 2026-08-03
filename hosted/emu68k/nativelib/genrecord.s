; genrecord.s - production proof for generated terminated-record arrays.
;
; A classic 20-byte NewMenu array is rebuilt as native 40-byte records,
; including its guest string pointers and opaque user-data cookie. The native
; Menu result crosses back as a typed token and FreeMenus consumes it.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
DOS_PutStr         equ -948
GT_CreateMenusA    equ -48
GT_FreeMenus       equ -54

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
    move.l  d0,a4

    move.l  a4,a6
    lea     newmenus(pc),a0
    suba.l  a1,a1
    jsr     GT_CreateMenusA(a6)
    tst.l   d0
    beq.s   failed

    move.l  d0,a0
    jsr     GT_FreeMenus(a6)

    move.l  a4,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)

    lea     passmsg(pc),a0
    bra.s   say
failed:
    lea     failmsg(pc),a0
say:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:      dc.b "dos.library",0
gadtoolsname: dc.b "gadtools.library",0
title:        dc.b "Bridge",0
item:         dc.b "Imported menu",0
key:          dc.b "I",0
passmsg:      dc.b "[T3RECORD] PASS",10,0
failmsg:      dc.b "[T3RECORD] FAIL",10,0
    even

; Classic packed-to-two NewMenu layout: 20 bytes per record.
newmenus:
    dc.b 1,0                       ; NM_TITLE, alignment pad
    dc.l title,0                   ; nm_Label, nm_CommKey
    dc.w 0                         ; nm_Flags
    dc.l 0,$12345678               ; nm_MutualExclude, nm_UserData

    dc.b 2,0                       ; NM_ITEM, alignment pad
    dc.l item,key                  ; nm_Label, nm_CommKey
    dc.w 0
    dc.l 0,$89abcdef

    dc.b 0,0                       ; NM_END sentinel is itself a record
    dc.l 0,0
    dc.w 0
    dc.l 0,0
