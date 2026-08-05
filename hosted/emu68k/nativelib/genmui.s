; genmui.s - behavioral proof for guest muimaster.library.
;
; Opening the library alone proves only its resident/init path.  Creating a
; built-in Text.mui object crosses the guest MUI class registry into native
; Intuition/BOOPSI and returns a guest-addressable facade; disposing it proves
; the reverse lifetime edge as well.

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_PutStr          equ -948
MUI_NewObjectA      equ -30
MUI_DisposeObject   equ -36

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.b   done
    move.l  d0,d7

    move.l  4.w,a6
    lea     muiname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.b   failed
    move.l  d0,d6

    move.l  d6,a6
    lea     textclass(pc),a0
    lea     tags(pc),a1
    jsr     MUI_NewObjectA(a6)
    tst.l   d0
    beq.b   fail_close_mui
    move.l  d0,a0
    move.l  d6,a6
    jsr     MUI_DisposeObject(a6)

    move.l  d7,a6
    lea     passmsg(pc),a0
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
    bra.b   close_mui

fail_close_mui:
    move.l  d7,a6
    lea     failmsg(pc),a0
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
close_mui:
    move.l  4.w,a6
    move.l  d6,a1
    jsr     EXEC_CloseLibrary(a6)
    bra.b   done

failed:
    move.l  d7,a6
    lea     failmsg(pc),a0
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:    dc.b "dos.library",0
muiname:    dc.b "muimaster.library",0
textclass:  dc.b "Text.mui",0
passmsg:    dc.b "[T3MUI] PASS",10,0
failmsg:    dc.b "[T3MUI] FAIL",10,0
    even
tags:
    dc.l 0,0
