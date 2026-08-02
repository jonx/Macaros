; genhook.s - native utility.library calls back into this program's 68k Hook.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
DOS_PutStr        equ -948
UTIL_CallHookPkt  equ -102

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     done
    move.l  d0,a5

    move.l  4.w,a6
    lea     utilname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a4

    move.l  a4,a6
    lea     hook(pc),a0
    lea     object(pc),a2
    lea     message(pc),a1
    jsr     UTIL_CallHookPkt(a6)
    cmp.l   #$3a,d0                ; $12 object + $23 message + 5 h_Data
    bne     failed

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

; Amiga Hook ABI on callback entry: A0=Hook, A2=object, A1=message.
hookentry:
    move.l  (a2),d0
    add.l   (a1),d0
    add.l   16(a0),d0
    rts

dosname:  dc.b "dos.library",0
utilname: dc.b "utility.library",0
passmsg:  dc.b "[T3HOOK] PASS",10,0
failmsg:  dc.b "[T3HOOK] FAIL",10,0
    even
hook:
    dc.l 0,0                       ; MinNode
    dc.l hookentry,0,5             ; h_Entry, h_SubEntry, h_Data
object:  dc.l $12
message: dc.l $23
