; geniff.s - iffparse driven end to end (LVOs taken from iffparse.conf,
; not from memory: the AROS vector order is its own), for the guest-side routing of the
; library itself. Writes a small FORM into a file and reads it back, so the
; whole path is exercised: AllocIFF, InitIFFasDOS, OpenIFF for write, a
; PushChunk/WriteChunkBytes/PopChunk pair, then a second pass that parses the
; file and checks the chunk it finds is the chunk that was written.
;
; The interesting part is not iffparse: it is what iffparse calls while it
; runs. Bridged, this exercises the crossings; routed guest-side, the library
; is real 68k code and only its own calls downward cross.

EXEC_OpenLibrary     equ -552
EXEC_CloseLibrary    equ -414
DOS_Open             equ -30
DOS_Close            equ -36
DOS_PutStr           equ -948
DOS_DeleteFile       equ -72
IFF_AllocIFF         equ -30
IFF_OpenIFF          equ -36
IFF_ParseIFF         equ -42
IFF_CloseIFF         equ -48
IFF_FreeIFF          equ -54
IFF_PushChunk        equ -84
IFF_PopChunk         equ -90
IFF_WriteChunkBytes  equ -66
IFF_ReadChunkBytes   equ -60
IFF_InitIFFasDOS     equ -234
IFF_CurrentChunk     equ -174
IFF_StopChunk        equ -126

MODE_NEWFILE         equ 1006
MODE_OLDFILE         equ 1005
IFFF_WRITE           equ 1
IFFPARSE_SCAN        equ 0
ID_FORM              equ $464F524D          ; 'FORM'
ID_TEST              equ $54455354          ; 'TEST'
ID_DATA              equ $44415441          ; 'DATA'

; struct IFFHandle { IPTR iff_Stream; ULONG iff_Flags; LONG iff_Depth; }
IFF_STREAM           equ 0

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5                  ; a5 = DOSBase

    move.l  4.w,a6
    lea     iffname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a4                  ; a4 = IFFParseBase

; ---- write pass -------------------------------------------------------
    move.l  a4,a6
    jsr     IFF_AllocIFF(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a3                  ; a3 = IFFHandle

    move.l  a5,a6
    lea     filename(pc),a0
    move.l  a0,d1
    move.l  #MODE_NEWFILE,d2
    jsr     DOS_Open(a6)
    tst.l   d0
    beq.w   freeiff
    move.l  d0,d7                  ; d7 = write handle
    move.l  d7,IFF_STREAM(a3)

    move.l  a4,a6
    move.l  a3,a0
    jsr     IFF_InitIFFasDOS(a6)

    move.l  a3,a0
    move.l  #IFFF_WRITE,d0
    jsr     IFF_OpenIFF(a6)
    tst.l   d0
    bne.w   closefile

    move.l  a3,a0
    move.l  #ID_TEST,d0            ; type = the FORM's type
    move.l  #ID_FORM,d1            ; id   = FORM
    moveq   #-1,d2
    jsr     IFF_PushChunk(a6)
    tst.l   d0
    bne.w   closeiff

    move.l  a3,a0
    moveq   #0,d0                  ; a plain chunk has no type
    move.l  #ID_DATA,d1
    moveq   #-1,d2
    jsr     IFF_PushChunk(a6)
    tst.l   d0
    bne.w   closeiff

    move.l  a3,a0
    lea     payload(pc),a1
    moveq   #8,d0                 ; WriteChunkBytes: A0=iff, A1=buf, D0=len
    jsr     IFF_WriteChunkBytes(a6)
    cmp.l   #8,d0
    bne.w   closeiff

    move.l  a3,a0
    jsr     IFF_PopChunk(a6)
    move.l  a3,a0
    jsr     IFF_PopChunk(a6)

    move.l  a3,a0
    jsr     IFF_CloseIFF(a6)
    move.l  a3,a0
    jsr     IFF_FreeIFF(a6)

    move.l  a5,a6
    move.l  d7,d1
    jsr     DOS_Close(a6)
    moveq   #0,d7

; ---- read pass --------------------------------------------------------
    move.l  a4,a6
    jsr     IFF_AllocIFF(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a3

    move.l  a5,a6
    lea     filename(pc),a0
    move.l  a0,d1
    move.l  #MODE_OLDFILE,d2
    jsr     DOS_Open(a6)
    tst.l   d0
    beq.w   freeiff
    move.l  d0,d7
    move.l  d7,IFF_STREAM(a3)

    move.l  a4,a6
    move.l  a3,a0
    jsr     IFF_InitIFFasDOS(a6)

    move.l  a3,a0
    moveq   #0,d0
    jsr     IFF_OpenIFF(a6)
    tst.l   d0
    bne.w   closefile

    move.l  a3,a0
    move.l  #ID_TEST,d0
    move.l  #ID_DATA,d1
    jsr     IFF_StopChunk(a6)
    tst.l   d0
    bne.w   closeiff

    move.l  a3,a0
    move.l  #IFFPARSE_SCAN,d0
    jsr     IFF_ParseIFF(a6)
    tst.l   d0
    bne.w   closeiff

    move.l  a3,a0
    jsr     IFF_CurrentChunk(a6)
    tst.l   d0
    beq.w   closeiff

    move.l  a3,a0
    lea     readbuf(pc),a1
    moveq   #8,d0                 ; ReadChunkBytes: A0=iff, A1=buf, D0=len
    jsr     IFF_ReadChunkBytes(a6)
    cmp.l   #8,d0
    bne.w   closeiff

    lea     payload(pc),a0
    lea     readbuf(pc),a1
    moveq   #7,d6
cmploop:
    move.b  (a0)+,d0
    cmp.b   (a1)+,d0
    bne.w   closeiff
    dbra    d6,cmploop

    move.l  a4,a6
    move.l  a3,a0
    jsr     IFF_CloseIFF(a6)
    move.l  a3,a0
    jsr     IFF_FreeIFF(a6)
    move.l  a5,a6
    move.l  d7,d1
    jsr     DOS_Close(a6)
    lea     filename(pc),a0
    move.l  a0,d1
    jsr     DOS_DeleteFile(a6)

    lea     passmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
    bra.w   done

closeiff:
    move.l  a4,a6
    move.l  a3,a0
    jsr     IFF_CloseIFF(a6)
closefile:
    tst.l   d7
    beq.b   freeiff
    move.l  a5,a6
    move.l  d7,d1
    jsr     DOS_Close(a6)
freeiff:
    move.l  a4,a6
    move.l  a3,a0
    jsr     IFF_FreeIFF(a6)
failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:   dc.b "dos.library",0
iffname:   dc.b "iffparse.library",0
filename:  dc.b "MacRW:geniff.iff",0
payload:   dc.b "IFFBYTES"
passmsg:   dc.b "[T3IFF] PASS",10,0
failmsg:   dc.b "[T3IFF] FAIL",10,0
    even

readbuf:   ds.b 8
