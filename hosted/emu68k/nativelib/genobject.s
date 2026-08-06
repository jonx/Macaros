; genobject.s - production proof for policy-compiled opaque OS objects.
;
; Locale and Catalog are native 64-bit pointers. The guest receives typed
; 32-bit tokens, hands them back through generated calls, and closes them.
; Opening the same catalog twice must preserve identity while retaining two
; close references. No vector below has a hand-written oscall case.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
DOS_PutStr        equ -948
LOC_CloseCatalog equ -36
LOC_CloseLocale  equ -42
LOC_ConvToUpper  equ -54
LOC_GetCatalogStr equ -72
LOC_GetLocaleStr equ -78
LOC_OpenCatalogA equ -150
LOC_OpenLocale   equ -156
INT_LockPubScreen   equ -510
INT_UnlockPubScreen equ -516
GFX_GetRGB4         equ -582
GFX_GetAPen         equ -858
SCREEN_COLORMAP     equ 48
SCREEN_RASTPORT     equ 84

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     done
    move.l  d0,a5

    move.l  4.w,a6
    lea     localename(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a4

    ; OpenLocale(NULL) -> typed Locale token.
    move.l  a4,a6
    suba.l  a0,a0
    jsr     LOC_OpenLocale(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d6
    and.l   #$ffff0000,d0
    cmp.l   #$e6800000,d0
    bne     failed

    ; The same token must recover the native Locale for another generated LVO.
    move.l  d6,a0
    moveq   #'a',d0
    jsr     LOC_ConvToUpper(a6)
    cmp.l   #'A',d0
    bne     failed

    ; Native locale strings cannot be truncated into D0. The generated
    ; crossing copies the bounded C string into guest memory and returns that
    ; guest address. DAY_1 is locale-dependent, but it must be non-empty.
    move.l  d6,a0
    moveq   #1,d0                  ; DAY_1
    jsr     LOC_GetLocaleStr(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a0
    tst.b   (a0)
    beq     failed

    move.l  d6,a0
    jsr     LOC_CloseLocale(a6)

    ; Open one real installed catalog twice. Native locale.library caches it;
    ; the bridge must expose that stable identity and keep two close refs.
    suba.l  a0,a0
    lea     catalogname(pc),a1
    lea     catalogtags(pc),a2
    jsr     LOC_OpenCatalogA(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d6

    suba.l  a0,a0
    lea     catalogname(pc),a1
    lea     catalogtags(pc),a2
    jsr     LOC_OpenCatalogA(a6)
    cmp.l   d6,d0
    bne     failed

    ; A missing catalog id returns the caller's default string. Even when the
    ; native result aliases guest input, the result policy returns a readable
    ; guest string rather than a 64-bit host pointer.
    move.l  d6,a0
    move.l  #$7fffffff,d0
    lea     fallback(pc),a1
    jsr     LOC_GetCatalogStr(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a0
    cmpi.l  #$66616c6c,(a0)       ; "fall"
    bne     failed
    cmpi.l  #$6261636b,4(a0)      ; "back"
    bne     failed
    tst.b   8(a0)
    bne     failed

    move.l  d6,a0
    jsr     LOC_CloseCatalog(a6)
    move.l  d6,a0
    jsr     LOC_CloseCatalog(a6)

    ; A public Screen and its nested ColorMap are borrowed native objects.
    ; Unlocking ends the OS lock but does not destroy either object. Classic
    ; software commonly retains that ColorMap pointer for an immediate query,
    ; so the typed identity must remain resolvable after the release.
    move.l  4.w,a6
    lea     intuitionname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d7

    move.l  4.w,a6
    lea     graphicsname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d5

    move.l  d7,a6
    suba.l  a0,a0
    jsr     INT_LockPubScreen(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d4
    move.l  d0,a0
    move.l  SCREEN_COLORMAP(a0),d3
    tst.l   d3
    beq     failed
    lea     SCREEN_RASTPORT(a0),a2
    suba.l  a0,a0
    move.l  d4,a1
    jsr     INT_UnlockPubScreen(a6)

    move.l  d5,a6
    move.l  d3,a0
    moveq   #0,d0
    jsr     GFX_GetRGB4(a6)
    move.l  a2,a0
    jsr     GFX_GetAPen(a6)

    move.l  4.w,a6
    move.l  d5,a1
    jsr     EXEC_CloseLibrary(a6)
    move.l  4.w,a6
    move.l  d7,a1
    jsr     EXEC_CloseLibrary(a6)
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

dosname:     dc.b "dos.library",0
localename:  dc.b "locale.library",0
intuitionname: dc.b "intuition.library",0
graphicsname: dc.b "graphics.library",0
catalogname: dc.b "System/Libs/dos.catalog",0
language:    dc.b "czech",0
fallback:    dc.b "fallback",0
passmsg:     dc.b "[T3OBJ] PASS",10,0
failmsg:     dc.b "[T3OBJ] FAIL",10,0
    even
catalogtags:
    dc.l $80090004,language          ; OC_Language, CSTR
    dc.l 0,0
