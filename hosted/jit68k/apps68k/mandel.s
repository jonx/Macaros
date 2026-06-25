; mandel.s — a fixed-point integer Mandelbrot-set ASCII renderer. A REAL big-endian
; AmigaOS hunk executable (vasmm68k_mot -Fhunkexe -no-opt) — the [J5j] capability
; CAPSTONE for the 68k JIT: a SUBSTANTIAL, recognisable real 68k program (not a
; unit-test-shaped micro-block) that stresses the translator with deeply nested loops,
; fixed-point signed multiplies, shifts, the full integer-compare/branch set, and heavy
; library output. It prints the classic Mandelbrot fractal row by row through PutChar.
;
; Assembled with -no-opt so vasm keeps the exact opcodes (it would otherwise fold
; addi->addq, move.l #small->moveq, etc.), so the *.exe drives the precise instruction
; set the JIT + the independent oracle each decode.
;
; ============================ THE MATH (fixed point, Q11) =======================
; The Mandelbrot iteration over complex c = cx + i*cy starting from z = 0:
;       z := z^2 + c          (z = zx + i*zy)
;   zx' = zx^2 - zy^2 + cx
;   zy' = 2*zx*zy     + cy
; escapes when |z|^2 = zx^2 + zy^2 > 4. The escape iteration count -> an ASCII shade.
;
; Everything is Q11 fixed point (1.0 == 2048 == $0800). Multiplies use muls.w (the
; 16x16 -> 32 signed multiply the JIT covers): for two Q11 values a,b the raw product
; (i16)a*(i16)b is Q22, shifted right 11 (asr.l #8 ; asr.l #3) back to Q11. With the
; escape radius |z| <= 2 the iterating zx/zy stay within ~+-6.5 (one update past the
; boundary) = ~+-13312, comfortably inside signed-16 so muls.w never overflows its
; 16-bit inputs, and the Q22 products fit a 32-bit longword. The escape test at the top
; of the loop fires before the values can grow further.
;
; Grid: 64 columns x 26 rows; cx in [-2.50, +1.00], cy in [-1.25, +1.25]; maxiter 32.
; ~50k inner iterations, each with three muls.w + shifts + adds + compares + branches +
; (d16,a5) memory loads/stores, plus 64*26 PutChar calls + 26 newlines through the [J3]
; library bridge — heavy enough to exercise opcode/addressing/flag combinations the unit
; tests never reach, finishing in well under a second through the JIT.
;
; OUTPUT: 26 lines of 64 chars each (+ '\n'), the recognisable Mandelbrot silhouette.
; The harness captures the PutChar byte stream and asserts it byte-exact between the
; JIT and the independent interpreter (and prints it so a human sees the fractal).
;
; The loop-invariant cell coordinates and the per-iteration squared terms live in a small
; SCRATCH FRAME in free sandbox space, reached through a5 with the (d16,a5) addressing
; mode (load AND store) — so the program leans on the [J5g] displacement-EA path heavily
; on top of the register/ALU/branch/multiply set. Add/sub/cmp use only register operands
; (the values are loaded from the frame into a data register first), staying inside the
; JIT's proven ALU subset.
;
; CALLING CONVENTION: a6 = the (stub) library base; PutChar is jsr LVO_PutChar(a6) with
; the character in d0 (the negative-offset LVO bridge, grounded in [J3]/stublib).
; ===============================================================================

SHIFT       equ     11                   ; Q11 fixed point: 1.0 = 2048
ONE         equ     2048                 ; 1.0 in Q11
FOUR        equ     8192                 ; 4.0 in Q11 (escape: zx^2 + zy^2 > 4)

WIDTH       equ     64
HEIGHT      equ     26
MAXITER     equ     32

; Q11 view window. cx steps from X0 by DX per column; cy from Y0 by DY per row.
;   X0 = -2.50*2048 = -5120 ;  DX = 3.50/64 *2048 = 112
;   Y0 = -1.25*2048 = -2560 ;  DY = 2.50/26 *2048 ~= 196
X0          equ     -5120
DX          equ     112
Y0          equ     -2560
DY          equ     196

LVO_PutChar equ     -30                  ; stub "print" sink (records the byte)

; ----- the scratch frame (free sandbox space above the code, below the lib base) -----
SCRATCH     equ     $00220000
F_CX        equ     0                    ; (0,a5)  = cx  (Q11) loop-invariant per column
F_CY        equ     4                    ; (4,a5)  = cy  (Q11) loop-invariant per row
F_PX        equ     8                    ; (8,a5)  = px  (column counter)
F_PY        equ     12                   ; (12,a5) = py  (row counter)
F_ZX2       equ     16                   ; (16,a5) = zx^2 (Q11)
F_ZY2       equ     20                   ; (20,a5) = zy^2 (Q11)

; ----- register use inside the iteration: d3=zx, d2=zy, d1=iter, d0=scratch -----

;------------------------------------------------------------------- entry
        move.l  #SCRATCH,a5          ; a5 -> the scratch frame

        move.l  #Y0,d0
        move.l  d0,F_CY(a5)         ; cy = Y0
        moveq   #0,d0
        move.l  d0,F_PY(a5)         ; py = 0

row_loop:
        move.l  #X0,d0
        move.l  d0,F_CX(a5)        ; cx = X0 (reset each row)
        moveq   #0,d0
        move.l  d0,F_PX(a5)        ; px = 0

col_loop:
        ; ---- iterate z := z^2 + c for the current cell ----
        moveq   #0,d3               ; zx = 0
        moveq   #0,d2               ; zy = 0
        moveq   #0,d1               ; iter = 0

iter_loop:
        ; zx2 = (zx*zx) >> SHIFT      (Q11)  -> store at F_ZX2(a5)
        move.l  d3,d0
        muls.w  d3,d0               ; d0 = (i16)zx * (i16)zx   (Q22)
        asr.l   #8,d0
        asr.l   #3,d0               ; d0 >>= 11  -> zx2 (Q11)
        move.l  d0,F_ZX2(a5)       ; store zx2  (move.l Dn,(d16,An))

        ; zy2 = (zy*zy) >> SHIFT      (Q11)  -> store at F_ZY2(a5)
        move.l  d2,d0
        muls.w  d2,d0               ; d0 = zy*zy (Q22)
        asr.l   #8,d0
        asr.l   #3,d0               ; -> zy2 (Q11)
        move.l  d0,F_ZY2(a5)       ; store zy2

        ; mag = zx2 + zy2 ; escape if mag > FOUR
        move.l  F_ZX2(a5),d0       ; d0 = zx2   (move.l (d16,An),Dn)
        move.l  F_ZY2(a5),d4       ; d4 = zy2
        add.l   d4,d0               ; d0 = zx2 + zy2
        cmp.l   #FOUR,d0            ; mag > 4 ?
        bgt     escaped             ; |z| escaped -> stop, iter = escape count

        ; zxy = (zx*zy) >> SHIFT, then *2     (2*zx*zy, Q11)
        move.l  d3,d0
        muls.w  d2,d0               ; d0 = zx*zy (Q22)
        asr.l   #8,d0
        asr.l   #3,d0               ; -> zx*zy (Q11)
        add.l   d0,d0               ; *2 -> 2*zx*zy (Q11)
        move.l  F_CY(a5),d4        ; d4 = cy
        add.l   d4,d0               ; + cy
        move.l  d0,d2               ; zy' = 2*zx*zy + cy

        ; zx' = zx2 - zy2 + cx
        move.l  F_ZX2(a5),d0       ; d0 = zx2
        move.l  F_ZY2(a5),d4       ; d4 = zy2
        sub.l   d4,d0               ; d0 = zx2 - zy2
        move.l  F_CX(a5),d4        ; d4 = cx
        add.l   d4,d0               ; + cx
        move.l  d0,d3               ; zx' = zx2 - zy2 + cx

        addq.l  #1,d1               ; iter++
        cmp.l   #MAXITER,d1
        bne     iter_loop           ; while iter != MAXITER (bne.w: target out of byte range)

        ; fell through without escaping -> inside the set. iter == MAXITER here.
escaped:
        ; ---- map the escape count in d1 to an ASCII shade and PutChar it ----
        ;   d1 == MAXITER -> '#' (inside the set)
        ;   d1 >= 12      -> '+'
        ;   d1 >= 6       -> '-'
        ;   d1 >= 3       -> '.'
        ;   else          -> ' '  (escaped immediately = background)
        cmp.l   #MAXITER,d1
        bne.s   not_in
        moveq   #'#',d0
        bra.s   emit
not_in:
        cmp.l   #12,d1
        blt.s   lt12
        moveq   #'+',d0
        bra.s   emit
lt12:
        cmp.l   #6,d1
        blt.s   lt6
        moveq   #'-',d0
        bra.s   emit
lt6:
        cmp.l   #3,d1
        blt.s   lt3
        moveq   #'.',d0
        bra.s   emit
lt3:
        moveq   #' ',d0             ; background
emit:
        jsr     LVO_PutChar(a6)     ; print the cell character (d0)

        ; ---- advance the column: cx += DX ; px++ ----
        move.l  F_CX(a5),d0
        add.l   #DX,d0
        move.l  d0,F_CX(a5)        ; cx += DX
        move.l  F_PX(a5),d0
        addq.l  #1,d0
        move.l  d0,F_PX(a5)        ; px++
        cmp.l   #WIDTH,d0
        bne     col_loop            ; while px != WIDTH (bne.w)

        ; ---- end of row: emit a newline ----
        moveq   #10,d0              ; '\n'
        jsr     LVO_PutChar(a6)

        ; ---- advance the row: cy += DY ; py++ ----
        move.l  F_CY(a5),d0
        add.l   #DY,d0
        move.l  d0,F_CY(a5)        ; cy += DY
        move.l  F_PY(a5),d0
        addq.l  #1,d0
        move.l  d0,F_PY(a5)        ; py++
        cmp.l   #HEIGHT,d0
        bne     row_loop            ; while py != HEIGHT (bne.w)

        ; ---- done ----
        moveq   #0,d0               ; exit code 0
        rts                         ; top-level rts -> program exit
