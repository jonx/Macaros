/* j3_vector.c — [J3] vector recognition: the dispatch math (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Implements the negative-offset 68k library-vector arithmetic
 * the dispatcher uses to recognise "this 68k jsr lands in library L's vector n",
 * grounded against the REAL contract in
 *   arch/m68k-all/include/aros/cpu.h  (upstream tree):
 *     __AROS_GETJUMPVEC(lib,n) = &(((struct JumpVec *)(lib))[-(n)])    (line 82)
 *     LIB_VECTSIZE             = sizeof(struct JumpVec) == 6 on m68k    (lines 19,81)
 *
 * &(((JumpVec*)lib)[-n]) is lib + (-n)*sizeof(JumpVec) = lib - n*6 (target stride).
 * So vector n sits at byte address  lib - n*6, and recovering n from a jump-target
 * PC is  n = (lib - pc)/6, valid only when pc <= lib and (lib - pc) is a multiple
 * of 6. No Emu68 source is used here. */
#include "j3_jit68k.h"

uint32_t j3_vector_addr(uint32_t libbase, int n)
{
    /* __AROS_GETJUMPVEC(lib,n): lib - n*LIB_VECTSIZE, target stride = 6.
     * 68k addresses are 32-bit; this wraps in uint32_t exactly as on the 68k. */
    return libbase - (uint32_t)n * J3_M68K_LIB_VECTSIZE;
}

int j3_vector_recognise(uint32_t libbase, uint32_t pc)
{
    /* The vector table grows DOWNWARD from the base (negative offsets), so a valid
     * vector PC is at or below the base. */
    if (pc > libbase)
        return -1;                       /* above the base: not a negative vector */

    uint32_t delta = libbase - pc;       /* = n * 6 for a clean hit */
    if (delta % J3_M68K_LIB_VECTSIZE != 0)
        return -1;                       /* not on a 6-byte vector boundary */

    return (int)(delta / J3_M68K_LIB_VECTSIZE);
}
