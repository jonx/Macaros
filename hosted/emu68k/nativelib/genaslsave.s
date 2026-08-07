; genaslsave.s - native asl.library save-mode requester through the 68k bridge.
;
; This is deliberately interactive: automation opens it, verifies the Save
; title/defaults, then either accepts or cancels it.  On acceptance it also
; reads FileRequester.fr_File and fr_Drawer from the guest facade, proving that
; the native requester's public result fields were copied back as guest
; strings rather than exposed as 64-bit native pointers.

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_PutStr          equ -948
ASL_AllocAslRequest equ -48
ASL_FreeAslRequest  equ -54
ASL_AslRequest      equ -60

TAG_DONE             equ 0
ASL_FileRequest      equ 0
ASLFR_TitleText      equ $80080001
ASLFR_InitialFile    equ $80080008
ASLFR_InitialDrawer  equ $80080009
ASLFR_DoSaveMode     equ $8008002c

FR_FILE              equ 4
FR_DRAWER            equ 8

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     aslname(pc),a1
    moveq   #36,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a4

    move.l  a4,a6
    moveq   #ASL_FileRequest,d0
    lea     alloc_tags(pc),a0
    jsr     ASL_AllocAslRequest(a6)
    tst.l   d0
    beq.w   failed_close_asl
    move.l  d0,a3

    move.l  a3,a0
    suba.l  a1,a1
    jsr     ASL_AslRequest(a6)
    tst.l   d0
    beq.b   cancelled

    ; A successful request must return guest-readable, non-empty strings.
    move.l  FR_FILE(a3),d0
    beq.b   bad_result
    move.l  d0,a0
    tst.b   (a0)
    beq.b   bad_result
    move.l  FR_DRAWER(a3),d0
    beq.b   bad_result
    move.l  d0,a0
    tst.b   (a0)
    beq.b   bad_result
    lea     accepted_msg(pc),a0
    bra.b   report

cancelled:
    lea     cancelled_msg(pc),a0
    bra.b   report

bad_result:
    lea     bad_result_msg(pc),a0

report:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)

    move.l  a4,a6
    move.l  a3,a0
    jsr     ASL_FreeAslRequest(a6)

failed_close_asl:
    move.l  4.w,a6
    move.l  a4,a1
    jsr     EXEC_CloseLibrary(a6)
    bra.b   close_dos

failed:
    lea     failed_msg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)

close_dos:
    move.l  4.w,a6
    move.l  a5,a1
    jsr     EXEC_CloseLibrary(a6)

done:
    moveq   #0,d0
    rts

dosname:        dc.b "dos.library",0
aslname:        dc.b "asl.library",0
title:          dc.b "68k bridge Save requester",0
initial_file:   dc.b "bridge-save-test.txt",0
initial_drawer: dc.b "RAM:",0
accepted_msg:   dc.b "[T3ASL] SAVE ACCEPTED",10,0
cancelled_msg:  dc.b "[T3ASL] SAVE CANCELLED",10,0
bad_result_msg: dc.b "[T3ASL] FAIL: invalid FileRequester result",10,0
failed_msg:     dc.b "[T3ASL] FAIL: asl.library unavailable",10,0
    even

alloc_tags:
    dc.l ASLFR_TitleText,title
    dc.l ASLFR_InitialFile,initial_file
    dc.l ASLFR_InitialDrawer,initial_drawer
    dc.l ASLFR_DoSaveMode,1
    dc.l TAG_DONE,0
