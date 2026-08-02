; autoinitlib.s - a genuine RTF_AUTOINIT 68k library test artifact.
;
; Unlike testlib.s (the direct-init resident), rt_Init below points to the
; classic four-long AutoInit table. A real loader must allocate the negative
; vector area + positive base, build six-byte vectors from functable, then call
; initfunc with D0=base, A0=seglist and A6=SysBase. Jumping to rt_Init as code is
; therefore guaranteed to fail this test: it is data, deliberately.

RTC_MATCHWORD   equ $4AFC
NT_LIBRARY      equ 9
RTF_AUTOINIT    equ $80
LIB_SIZE        equ 64
INIT_MARK_OFF   equ 56
STRUCT_MARK_OFF equ 52
SEGLIST_OFF     equ 48
LIB_OPENCNT     equ 32

        moveq   #-1,d0
        rts

        cnop    0,4
romtag:
        dc.w    RTC_MATCHWORD
        dc.l    romtag
        dc.l    endskip
        dc.b    RTF_AUTOINIT
        dc.b    1
        dc.b    NT_LIBRARY
        dc.b    0
        dc.l    libname
        dc.l    libid
        dc.l    inittab                 ; DATA, not executable code

; A second named resident in the same hunk exercises requested-name selection
; and the other legal MakeFunctions table encoding.
        cnop    0,4
romtag_abs:
        dc.w    RTC_MATCHWORD
        dc.l    romtag_abs
        dc.l    endskip
        dc.b    RTF_AUTOINIT
        dc.b    1
        dc.b    NT_LIBRARY
        dc.b    0
        dc.l    libname_abs
        dc.l    libid_abs
        dc.l    inittab_abs

; struct init { ULONG dSize; APTR vectors; APTR structure; ULONG_FUNC init; }
inittab:
        dc.l    LIB_SIZE
        dc.l    functable
        dc.l    structinit
        dc.l    initfunc

inittab_abs:
        dc.l    LIB_SIZE
        dc.l    functable_abs
        dc.l    structinit
        dc.l    initfunc

; Classic big-endian InitStruct stream: copy one LONG to byte offset 52, then
; terminate. This is INITLONG(STRUCT_MARK_OFF,$1a1757c7) written explicitly.
structinit:
        dc.w    $c000
        dc.w    STRUCT_MARK_OFF
        dc.l    $1a1757c7
        dc.w    0

; Classic relative-offset MakeFunctions table. The first -1 selects relative
; mode; every signed WORD is based at the address of that first marker.
functable:
        dc.w    -1
        dc.w    libopen-functable
        dc.w    libclose-functable
        dc.w    libexpunge-functable
        dc.w    libreserved-functable
        dc.w    testadd-functable
        dc.w    testmagic-functable
        dc.w    -1

; The alternate resident uses the absolute-pointer form.
functable_abs:
        dc.l    libopen
        dc.l    libclose
        dc.l    libexpunge
        dc.l    libreserved
        dc.l    testadd
        dc.l    testmagic
        dc.l    -1

; AutoInit final callback. Preserve D0=base and leave a value in the positive
; area so the harness proves this code ran after MakeLibrary/InitStruct.
initfunc:
        move.l  d0,a1
        move.l  a0,SEGLIST_OFF(a1)
        move.l  #$a17e1a17,INIT_MARK_OFF(a1)
        rts

libopen:
        addq.w  #1,LIB_OPENCNT(a6)
        move.l  a6,d0
        rts
libclose:
        subq.w  #1,LIB_OPENCNT(a6)
        bne.s   stillopen
        move.l  SEGLIST_OFF(a6),d0
        rts
stillopen:
        moveq   #0,d0
        rts
libexpunge:
        move.l  SEGLIST_OFF(a6),d0
        rts
libreserved:
        moveq   #0,d0
        rts

testadd:
        add.l   d1,d0
        rts
testmagic:
        move.l  #$a170c0de,d0
        rts

libname:
        dc.b    "autoinit.library",0
libid:
        dc.b    "autoinit.library 1.0 (2026)",13,10,0
libname_abs:
        dc.b    "autoinitabs.library",0
libid_abs:
        dc.b    "autoinitabs.library 1.0 (2026)",13,10,0
        cnop    0,4
endskip:
