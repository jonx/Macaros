; genfacade.s - a native DiskObject exposed as a guest-readable safe facade.

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_PutStr          equ -948
ICON_FreeDiskObject equ -90
ICON_DupDiskObjectA equ -150
ICON_NewDiskObject  equ -174

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     done
    move.l  d0,a5

    move.l  4.w,a6
    lea     iconname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a4

    move.l  a4,a6
    moveq   #3,d0                  ; WBTOOL
    jsr     ICON_NewDiskObject(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d6
    cmp.l   #$00210000,d0          ; a real address in guest RAM, not a token
    bcs     failed
    cmp.l   #$02000000,d0
    bcc     failed
    move.l  d0,a0
    cmp.b   #3,48(a0)              ; DiskObject.do_Type is guest-readable
    bne     failed
    tst.l   50(a0)                 ; unsupported native STRPTR fails closed
    bne     failed

    ; A per-library tag domain drives a second native-owned facade.
    move.l  d6,a0
    lea     duptags(pc),a1
    jsr     ICON_DupDiskObjectA(a6)
    tst.l   d0
    beq     failed
    cmp.l   d6,d0                  ; duplicate is a distinct native identity
    beq     failed
    move.l  d0,d7
    move.l  d0,a0
    cmp.b   #3,48(a0)
    bne     failed
    jsr     ICON_FreeDiskObject(a6)

    move.l  d6,a0
    jsr     ICON_FreeDiskObject(a6)

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

dosname:  dc.b "dos.library",0
iconname: dc.b "icon.library",0
passmsg:  dc.b "[T3FACADE] PASS",10,0
failmsg:  dc.b "[T3FACADE] FAIL",10,0
    even
duptags:
    dc.l $8000903f,0              ; ICONDUPA_DuplicateDefaultTool = FALSE
    dc.l 0,0
