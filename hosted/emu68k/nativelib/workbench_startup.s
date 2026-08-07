; Workbench startup mirror regression fixture.
;
; The host harness switches this run to Workbench mode before instruction
; zero.  Verify the classic process flag, dequeue the startup message from the
; embedded process port, and inspect both big-endian WBArg entries.

EXEC_FindTask equ -294
EXEC_GetMsg   equ -372

PR_MSGPORT    equ 92
PR_CLI        equ 172
WBS_NUMARGS   equ 28
WBS_ARGLIST   equ 36
WBARG_SIZE    equ 8

    move.l  4.w,a6
    moveq   #0,d0
    move.l  d0,a1
    jsr     EXEC_FindTask(a6)
    move.l  d0,a2
    tst.l   d0
    beq.s   fail1
    tst.l   PR_CLI(a2)
    bne.s   fail2

    lea     PR_MSGPORT(a2),a0
    jsr     EXEC_GetMsg(a6)
    move.l  d0,a3
    tst.l   d0
    beq.s   fail3
    cmp.w   #40,18(a3)
    bne.s   fail4
    cmp.l   #2,WBS_NUMARGS(a3)
    bne.s   fail5
    move.l  WBS_ARGLIST(a3),a4
    move.l  a4,d0
    tst.l   d0
    beq.s   fail6

    cmp.l   #$11111111,(a4)
    bne.s   fail7
    move.l  4(a4),a0
    cmp.l   #$576f7264,(a0)       ; "Word"
    bne.s   fail8
    cmp.l   #$22222222,WBARG_SIZE(a4)
    bne.s   fail9
    move.l  WBARG_SIZE+4(a4),a0
    cmp.l   #$50726f6a,(a0)       ; "Proj"
    bne.s   fail10

    moveq   #0,d0
    rts
fail1:
    moveq   #1,d0
    rts
fail2:
    moveq   #2,d0
    rts
fail3:
    moveq   #3,d0
    rts
fail4:
    moveq   #4,d0
    rts
fail5:
    moveq   #5,d0
    rts
fail6:
    moveq   #6,d0
    rts
fail7:
    moveq   #7,d0
    rts
fail8:
    moveq   #8,d0
    rts
fail9:
    moveq   #9,d0
    rts
fail10:
    moveq   #10,d0
    rts
