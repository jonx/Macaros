; [T3e] Live exec.OpenLibrary lifecycle test.
; Open a disk-loaded 68k library, call it, close/expunge it, then reopen it.
; Tail allocations are reclaimed safely, so a reload may reuse the same base.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
TEST_Add          equ -30
TEST_Magic        equ -36
CLONE_Serial      equ -30
CLONE_Magic       equ -36

        move.l  4.w,a6
        lea     libname(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        beq.w   failed
        move.l  d0,a5

        moveq   #20,d0
        moveq   #22,d1
        move.l  a5,a6
        jsr     TEST_Add(a6)
        cmpi.l  #42,d0
        bne.w   failed

        move.l  4.w,a6
        move.l  a5,a1
        jsr     EXEC_CloseLibrary(a6)

        move.l  4.w,a6
        lea     libname(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        beq.w   failed
        move.l  d0,a5

        ; A fresh initializer/Open ran even if safe tail reclaim reused the
        ; same address: the reconstructed base starts with open count one.
        cmpi.w  #1,32(a5)
        bne.w   failed

        move.l  a5,a6
        jsr     TEST_Magic(a6)
        cmpi.l  #$a170c0de,d0
        bne.w   failed

        move.l  4.w,a6
        move.l  a5,a1
        jsr     EXEC_CloseLibrary(a6)

        ; The non-AUTOINIT Resident path must work through this same live seam.
        lea     directname(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        beq.w   failed
        move.l  d0,a5
        moveq   #19,d0
        moveq   #23,d1
        move.l  a5,a6
        jsr     TEST_Add(a6)
        cmpi.l  #42,d0
        bne.w   failed
        move.l  a5,a1
        move.l  4.w,a6
        jsr     EXEC_CloseLibrary(a6)

        ; Version 2 must fail before executing a version-1 initializer.
        lea     libname(pc),a1
        moveq   #2,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        bne.w   failed

        ; A -> B -> A must be detected from LOADING state, fail normally, and
        ; restore the loader stack rather than leaving either base registered.
        lea     cyclename(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        bne.w   failed

        ; Neither the excessive-version probe nor the cycle may poison later
        ; valid opens. Load the ordinary library once more and close it.
        lea     libname(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        beq.w   failed
        move.l  d0,a1
        move.l  4.w,a6
        jsr     EXEC_CloseLibrary(a6)

        ; ---- a library that hands a different base to every opener ---------
        ; Two live opens must be two distinct, independently working bases, and
        ; each must be closable on its own.
        move.l  4.w,a6
        lea     clonename(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        beq.w   failed
        move.l  d0,a5                   ; first opener

        move.l  4.w,a6
        lea     clonename(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        beq.w   failed
        move.l  d0,a4                   ; second opener
        cmp.l   a5,a4
        beq.w   failed                  ; a clone base, not the same one twice

        move.l  a5,a6                   ; the vector table came with the clone
        jsr     CLONE_Magic(a6)
        cmpi.l  #$c10ec0de,d0
        bne.w   failed
        move.l  a5,a6
        jsr     CLONE_Serial(a6)
        cmpi.l  #1,d0
        bne.w   failed
        move.l  a4,a6
        jsr     CLONE_Serial(a6)
        cmpi.l  #2,d0
        bne.w   failed

        ; Closing the FIRST leaves the second working: one reference went, and
        ; Close ran on the base that was handed out, not on the library's own.
        move.l  4.w,a6
        move.l  a5,a1
        jsr     EXEC_CloseLibrary(a6)
        move.l  a4,a6
        jsr     CLONE_Serial(a6)
        cmpi.l  #2,d0
        bne.w   failed

        move.l  4.w,a6
        move.l  a4,a1
        jsr     EXEC_CloseLibrary(a6)   ; last one out: the expunge path

        ; And it reloads cleanly afterwards, serial counting from one again.
        move.l  4.w,a6
        lea     clonename(pc),a1
        moveq   #1,d0
        jsr     EXEC_OpenLibrary(a6)
        tst.l   d0
        beq.w   failed
        move.l  d0,a5
        move.l  a5,a6
        jsr     CLONE_Serial(a6)
        cmpi.l  #1,d0
        bne.w   failed
        move.l  4.w,a6
        move.l  a5,a1
        jsr     EXEC_CloseLibrary(a6)

        moveq   #0,d0
        rts
failed:
        moveq   #20,d0
        rts

libname:
        dc.b    "autoinit.library",0
cyclename:
        dc.b    "cyclea.library",0
directname:
        dc.b    "test.library",0
clonename:
        dc.b    "clone.library",0
        cnop    0,2
