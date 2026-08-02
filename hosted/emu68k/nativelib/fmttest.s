; fmttest.s - drive exec.RawDoFmt the way a real Amiga program does, and check
; the bytes it produced.
;
; The point is the CALLBACK: PutChProc below is this program's own 68k code,
; called once per character by RawDoFmt, with A3 advancing as it goes. That is
; the whole reason RawDoFmt runs in the guest rather than through the native
; bridge, so a test that does not use a real callback would prove nothing.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
EXEC_RawDoFmt     equ -522
DOS_PutStr        equ -948

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     bad
    move.l  d0,a5                       ; a5 = DOSBase

    ; ---- case 1: strings, decimals and hex in one pass -------------------
    lea     fmt1(pc),a0
    lea     args1(pc),a1
    lea     buffer(pc),a3
    lea     putch(pc),a2
    move.l  4.w,a6
    jsr     EXEC_RawDoFmt(a6)

    lea     buffer(pc),a0
    lea     want1(pc),a1
    bsr     strcmp
    tst.l   d0
    bne     bad

    ; ---- case 2: width, zero-pad, left-align, negatives ------------------
    lea     fmt2(pc),a0
    lea     args2(pc),a1
    lea     buffer(pc),a3
    lea     putch(pc),a2
    move.l  4.w,a6
    jsr     EXEC_RawDoFmt(a6)

    lea     buffer(pc),a0
    lea     want2(pc),a1
    bsr     strcmp
    tst.l   d0
    bne     bad

    lea     okmsg(pc),a0
    bra.s   say
bad:
    lea     badmsg(pc),a0
say:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
    move.l  a5,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    moveq   #0,d0
    rts

; the classic sprintf-style callback: one character, A3 advancing
putch:
    move.b  d0,(a3)+
    rts

; strcmp(a0,a1) -> d0 = 0 when equal
strcmp:
    move.b  (a0)+,d0
    move.b  (a1)+,d1
    cmp.b   d1,d0
    bne.s   .diff
    tst.b   d0
    bne.s   strcmp
    moveq   #0,d0
    rts
.diff:
    moveq   #1,d0
    rts

dosname: dc.b "dos.library",0
    even

fmt1:  dc.b "[%s] %ld %lx %c%%",0
    even
args1: dc.l str1
       dc.l -12345
       dc.l $beef
       dc.w 'Z'
want1: dc.b "[hello] -12345 beef Z%",0
    even

fmt2:  dc.b "|%6ld|%-6ld|%06ld|%4lx|%.3s|",0
    even
args2: dc.l 42
       dc.l 42
       dc.l -42
       dc.l $ab
       dc.l str1
want2: dc.b "|    42|42    |-00042|  ab|hel|",0
    even

str1:  dc.b "hello",0
okmsg: dc.b "[T3FMT] PASS",10,0
badmsg:dc.b "[T3FMT] FAIL",10,0
    even
buffer: ds.b 256
