/* j5d_ea_helpers.c — [J5d] HOOK 1: the (An)-class sandbox-memory EA emitter (OURS,
 * AROS-licensed). This is the disclosed [J5a] fix realised as a helper the darwinize-
 * rewritten build-dir copy of M68k_EA.c calls in place of its raw `ldr/str [reg_An]`.
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS.
 * It #includes the quarantined Emu68 emitter (emu68/A64.h) and CALLS its inline
 * encoders; per MPL-2.0 / Mozilla FAQ Q11-Q13 calling/linking does NOT relicense it.
 * No Emu68 source is copied here.
 *
 * ============================= WHY THIS EXISTS ==================================
 * The REAL Emu68 EA decoder emits, for `move.l (An),Dn` / `add.l (An)+,Dn` / etc:
 *     ldr_offset(reg_An, val, 0)            // (An)
 *     ldr_offset_postindex(reg_An, val, 4)  // (An)+  : load [reg_An], reg_An += 4
 * treating the An value as a RAW HOST POINTER (Emu68's 1:1 MMU) AND assuming the CPU
 * runs big-endian (Emu68 sets SCTLR.EE on bare metal). On our little-endian hosted
 * sandbox both are wrong. emu68_darwinize.pl rewrites each (An)-class site in the
 * BUILD-DIR COPY of M68k_EA.c (the quarantine stays byte-verbatim) to instead call:
 *     ptr = j5d_ea_mem(ptr, KIND, reg_An, val, index_amount);
 * which emits the sandbox-base-add + REV byteswap + the real load/store + the post/pre
 * index update on the 68k An register. KIND encodes {load|store, size 1/2/4, signed?,
 * index none|post|pre} — the darwinize script derives it mechanically from the original
 * encoder name. The QUARANTINE M68k_EA.c is unchanged; diff vs upstream is empty.
 *
 * ============================= THE EMITTED SEQUENCE =============================
 * Registers (the [J5d] engine seeds these in its block prologue; they are disjoint
 * from the Emu68 D/A map w13..w29 + REG_PC w18 and from the RA scratch pool w4..w11):
 *   x12  = sandbox base-adjust = (host_mem - origin)   [64-bit, set by the prologue]
 *   x0   = the M68KState* (RA reserves x0..x3; x0 holds state for the whole block)
 *   x3   = host-pointer scratch (RA-reserved, free for our fixed use)
 * For a load of size S at An:
 *   add  x3, x12, w_An, UXTW          // host = base_adjust + (uint32_t)An
 *   ldr{,h,b} w_val, [x3]             // raw little-endian host load (S bytes)
 *   rev / rev16 / (none) w_val        // byteswap to architectural 68k value
 *   sxth/sxtb w_val (if signed)       // sign-extend sub-longword if the EA asked
 * For a store of size S at An (the value to write is already in w_val, host order):
 *   rev / rev16 w_tmp = w_val         // architectural -> big-endian bytes
 *   add  x3, x12, w_An, UXTW
 *   str{,h,b} w_tmp, [x3]
 * For post-index (+S):  add  w_An, w_An, #S   (after the access; An is a 68k address)
 * For pre-index  (-S):  sub  w_An, w_An, #S   (BEFORE the access)
 *
 * NOTE: we do NOT bounds-check here (the [J5a] spike's OOB path) — the [J5d] engine
 * validates each (An) access stays in the sandbox in C before translating the block
 * (j5d_engine.c pre-decode pass), and the corpus's accesses are all in-range. A
 * production engine would emit the bounds compare here; documented as [J5d] scope.
 * ===============================================================================
 */
#include <stdint.h>
#include "emu68/A64.h"     /* adopted MPL emitter — encoders only, called not copied */

/* The kind code the darwinize transform passes. Bit layout (authored here, the script
 * mirrors it): bit0..1 = size selector (0=byte,1=half,2=word); bit2 = is_store;
 * bit3 = signed (sub-longword sign-extend on load); bit4..5 = index (0=none,1=post,
 * 2=pre). Index AMOUNT is the |offset| the original encoder used (1/2/4). */
#define J5D_EA_SZ(k)      ((k) & 3u)        /* 0=B 1=H 2=W */
#define J5D_EA_STORE(k)   (((k) >> 2) & 1u)
#define J5D_EA_SIGNED(k)  (((k) >> 3) & 1u)
#define J5D_EA_INDEX(k)   (((k) >> 4) & 3u) /* 0 none 1 post 2 pre */

/* Fixed registers the engine prologue seeds / reserves (see header comment). */
#define J5D_BASEADJ  12u    /* x12 = host_mem - origin (sandbox base-adjust)        */
#define J5D_HOSTPTR   3u    /* x3  = computed host pointer (RA-reserved scratch)    */

/* Count of (An)-class sandbox accesses the engine emitted — the engine reads this for
 * its stats so the test can assert real memory traffic went through the JIT. */
unsigned long g_j5d_ea_emits = 0;

/* Emit the sandbox-translated (An)-class access. `reg_An` is the AArch64 reg holding
 * the 68k An value; `val` is the value/dest reg (load: written; store: read). Returns
 * the advanced emit pointer. */
uint32_t *j5d_ea_mem(uint32_t *ptr, unsigned kind, uint8_t reg_An, uint8_t val, int index_amount)
{
    unsigned sz       = J5D_EA_SZ(kind);
    unsigned is_store = J5D_EA_STORE(kind);
    unsigned is_signed= J5D_EA_SIGNED(kind);
    unsigned index    = J5D_EA_INDEX(kind);
    unsigned amt      = (unsigned)(index_amount < 0 ? -index_amount : index_amount);

    g_j5d_ea_emits++;   /* one more (An)-class sandbox access went through the JIT */

    /* pre-decrement updates An BEFORE the access (68k -(An)). */
    if (index == 2)
        *ptr++ = sub_immed(reg_An, reg_An, (uint16_t)amt);

    /* host pointer = base_adjust + (uint32_t)An. */
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, reg_An, UXTW, 0);

    if (!is_store) {
        /* ---- LOAD ---- raw host load (little-endian), then byteswap to 68k value. */
        switch (sz) {
            case 2: /* word (.L) */
                *ptr++ = ldr_offset(J5D_HOSTPTR, val, 0);
                *ptr++ = rev(val, val);
                break;
            case 1: /* half (.W) */
                *ptr++ = ldrh_offset(J5D_HOSTPTR, val, 0);
                *ptr++ = rev16(val, val);
                if (is_signed) *ptr++ = sxth(val, val); else *ptr++ = uxth(val, val);
                break;
            default: /* byte (.B) — no byteswap */
                *ptr++ = ldrb_offset(J5D_HOSTPTR, val, 0);
                if (is_signed) *ptr++ = sxtb(val, val);
                break;
        }
    } else {
        /* ---- STORE ---- byteswap the host-order value into x2 (RA-reserved), store. */
        switch (sz) {
            case 2:
                *ptr++ = rev(2 /*w2 scratch*/, val);
                *ptr++ = str_offset(J5D_HOSTPTR, 2, 0);
                break;
            case 1:
                *ptr++ = rev16(2, val);
                *ptr++ = strh_offset(J5D_HOSTPTR, 2, 0);
                break;
            default:
                *ptr++ = strb_offset(J5D_HOSTPTR, val, 0);
                break;
        }
    }

    /* post-increment updates An AFTER the access (68k (An)+). */
    if (index == 1)
        *ptr++ = add_immed(reg_An, reg_An, (uint16_t)amt);

    return ptr;
}
