; [T3e] cycleb.library: its final initializer requires cyclea.library.

RTC_MATCHWORD  equ $4afc
RTF_AUTOINIT   equ $80
NT_LIBRARY     equ 9
OPENLIBRARY    equ -552
CLOSELIBRARY   equ -414

        moveq   #-1,d0
        rts
        cnop    0,4
resident:
        dc.w    RTC_MATCHWORD
        dc.l    resident
        dc.l    endskip
        dc.b    RTF_AUTOINIT,1,NT_LIBRARY,0
        dc.l    name,idstring,inittab
inittab:
        dc.l    40,functions,0,init
functions:
        dc.l    open,close,expunge,reserved,-1

init:
        move.l  d0,a5
        move.l  4.w,a6
        lea     dependency(pc),a1
        moveq   #1,d0
        jsr     OPENLIBRARY(a6)
        tst.l   d0
        beq.s   initfailed
        move.l  d0,a1
        move.l  4.w,a6
        jsr     CLOSELIBRARY(a6)
        move.l  a5,d0
        rts
initfailed:
        moveq   #0,d0
        rts
open:
        move.l  a6,d0
        rts
close:
reserved:
        moveq   #0,d0
        rts
expunge:
        move.l  36(a6),d0
        rts
dependency:
        dc.b    "cyclea.library",0
name:
        dc.b    "cycleb.library",0
idstring:
        dc.b    "cycleb.library 1.0",0
        cnop    0,4
endskip:
