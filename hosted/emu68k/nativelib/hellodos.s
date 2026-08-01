; hellodos.s - a REAL AmigaOS program: get SysBase from absolute 4, open
; dos.library through exec, then print with dos.library Output()/Write().
; This is the idiom every classic Amiga C program compiles down to, so making
; it work is what lets ordinary Amiga software call the native AROS libraries.
;
;   move.l 4.w,a6            SysBase (NOT passed in: read from address 4)
;   jsr    -552(a6)          OpenLibrary("dos.library", 0)
;   jsr     -60(a6)          Output()      -> D0 = stdout file handle
;   jsr     -48(a6)          Write(fh, buf, len)
;   jsr    -414(a6)          CloseLibrary

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_Write           equ -48
DOS_Output          equ -60

    move.l  4.w,a6                  ; SysBase from absolute address 4
    lea     dosname(pc),a1
    moveq   #0,d0                   ; any version
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   nodos
    move.l  d0,a5                   ; keep DOSBase in a5 (a6 is the call base)

    move.l  a5,a6
    jsr     DOS_Output(a6)          ; D0 = stdout handle
    move.l  d0,d1                   ; D1 = file handle
    beq.s   closeit
    lea     msg(pc),a0
    move.l  a0,d2                   ; D2 = buffer
    moveq   #msgend-msg,d3          ; D3 = length
    move.l  a5,a6
    jsr     DOS_Write(a6)

closeit:
    move.l  4.w,a6
    move.l  a5,a1
    jsr     EXEC_CloseLibrary(a6)
    moveq   #0,d0
    rts
nodos:
    moveq   #20,d0                  ; RETURN_FAIL
    rts

dosname:
    dc.b    "dos.library",0
    cnop    0,2
msg:
    dc.b    "Hello from a 68k program, printed by the native dos.library.",10
msgend:
    cnop    0,2
