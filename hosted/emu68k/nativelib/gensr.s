; gensr.s - what the engine does with the SR-writing instructions.
;
; The static scan routes a program away from translation the moment it sees
; `ORI to SR`, on the grounds that it is privileged. But privilege is about the
; real machine; the question here is only whether the ENGINE can serve it, and
; the interrupt-mask bits a user program sets have no meaning under translation.
; So this asks the engine directly, which is the only authority: set bits in the
; SR, clear them again, and check the condition codes still behave afterwards.

DOS_PutStr    equ -948
EXEC_OpenLibrary equ -552

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5

    ori     #$0700,sr            ; mask interrupts, as old startup code does
    andi    #$f8ff,sr            ; and unmask them again

    ; the CCR must still work after the SR round trip
    moveq   #1,d0
    subq.l  #1,d0
    bne.w   bad                  ; Z must be set

    lea     passmsg(pc),a0
    bra.w   say
bad
    lea     badmsg(pc),a0
say
    move.l  a5,a6
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
done
    moveq   #0,d0
    rts

passmsg dc.b "[T3SR] PASS",10,0
badmsg  dc.b "[T3SR] FAIL: the condition codes did not survive an SR write",10,0
dosname dc.b "dos.library",0
    even
