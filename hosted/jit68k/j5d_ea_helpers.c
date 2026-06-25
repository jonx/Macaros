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
 *   x1   = the M68KState* ([J5g]: RA reserves x0..x3; x1 holds state, w0 is free for
 *          Emu68's hardcoded cset(0,...) flag scratch); x3 = host-pointer scratch
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

/* ============================ [J5g] the FUNNEL-HELPER EA ============================
 * [J5d]'s j5d_ea_mem above bridges the DIRECT (An)/(An)+/-(An) sites (modes 2/3/4 in
 * M68k_EA.c). [J5g] BROADENS the sandbox-EA boundary to the displacement/indexed/
 * absolute/PC-relative modes, which the REAL Emu68 EA decoder routes through FOUR inline
 * funnel helpers — load_reg_from_addr_offset / load_reg_from_addr / store_reg_to_addr_
 * offset / store_reg_to_addr (M68k_EA.c). emu68_darwinize.pl --ea-sandbox now also
 * replaces the BODY of each of those four helpers in the BUILD-DIR COPY with a single
 * tail-call to one of the two emitters below. The DECODE (which mode, which registers,
 * the d16/d8/brief-extension-word extraction via cache_read_16, the index sign/scale) is
 * STILL 100% the REAL Emu68 decoder — only the final memory touch is sandbox-translated:
 *   host = sandbox_base_adjust(x12) + (uint32_t)addr68k   (UXTW)
 * plus the big-endian REV, exactly as j5d_ea_mem does for the (An) class. Because EVERY
 * memory mode (5 = (d16,An), 6 = (d8,An,Xn), 7.0 = abs.w, 7.1 = abs.l, 7.2 = (d16,PC),
 * 7.3 = (d8,PC,Xn)) funnels through these four helpers, one faithful substitution covers
 * them all uniformly. The QUARANTINE M68k_EA.c stays BYTE-VERBATIM (diff vs upstream
 * empty); only the build copy is patched, and the patch is a whole-helper-body
 * call-substitution disclosed in emu68/NOTICE.
 *
 * size == 0 is the "load EFFECTIVE ADDRESS" case (lea/pea/the EXG-mode): the helper must
 * compute the 68k address into `reg` and NOT touch memory / NOT add the host base / NOT
 * byteswap — the result is a 68k address consumed later. We honour that exactly.
 *
 * Fixed scratch (disjoint from the Emu68 D/A map w13..w29, REG_PC w18, the RA pool
 * w4..w11, and x0=state / x1 unused-in-block / x12=base_adjust): x2 (byteswap/addr temp)
 * and x3 (computed host pointer), both RA-reserved (j5c_ra.c hands out only w4..w11).
 * ===================================================================================*/

/* Emit the byteswapped sandbox LOAD/STORE through host pointer x3 (= base_adjust + addr).
 * `addr68k` is the AArch64 reg holding the final 32-bit 68k address. */
static uint32_t *ea_access(uint32_t *ptr, unsigned sz, unsigned is_store,
                           unsigned is_signed, uint8_t addr68k, uint8_t val)
{
    g_j5d_ea_emits++;
    /* host = base_adjust(x12) + (uint32_t)addr68k  -> x3 */
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, addr68k, UXTW, 0);

    if (!is_store) {
        switch (sz) {
            case 2: *ptr++ = ldr_offset(J5D_HOSTPTR, val, 0); *ptr++ = rev(val, val); break;
            case 1: *ptr++ = ldrh_offset(J5D_HOSTPTR, val, 0); *ptr++ = rev16(val, val);
                    if (is_signed) *ptr++ = sxth(val, val); else *ptr++ = uxth(val, val); break;
            default:*ptr++ = ldrb_offset(J5D_HOSTPTR, val, 0);
                    if (is_signed) *ptr++ = sxtb(val, val); break;
        }
    } else {
        switch (sz) {
            case 2: *ptr++ = rev(2 /*w2*/, val);  *ptr++ = str_offset(J5D_HOSTPTR, 2, 0); break;
            case 1: *ptr++ = rev16(2, val);       *ptr++ = strh_offset(J5D_HOSTPTR, 2, 0); break;
            default:*ptr++ = strb_offset(J5D_HOSTPTR, val, 0); break;
        }
    }
    return ptr;
}

/* The OFFSET funnel (modes 5 = (d16,An), 7.1 abs.l via a scratch base, 7.2 (d16,PC)).
 * Mirrors load_reg_from_addr_offset / store_reg_to_addr_offset: addr68k = base + offset.
 * `offset` is a signed displacement (16-bit for (d16,An); a 32-bit absolute/PC offset when
 * offset_32bit). KIND: bit0..1 size, bit2 store, bit3 signed (the darwinize transform
 * derives store/signed from the helper name + sign_ext arg). */
uint32_t *j5d_ea_addr_offset(uint32_t *ptr, unsigned kind, uint8_t base, uint8_t val,
                             int32_t offset, int offset_32bit)
{
    unsigned sz = kind & 3u, is_store = (kind >> 2) & 1u, is_signed = (kind >> 3) & 1u;
    uint8_t addr = 2; /* x2 holds the computed 32-bit 68k address */

    if (base == 0xff) {
        /* Emu68's "no base register" (absolute addressing): the address IS the offset
         * alone — materialise the 32-bit offset directly into x2, no base add. */
        *ptr++ = movw_immed_u16(addr, (uint16_t)(offset & 0xffff));
        if ((uint32_t)offset >> 16)
            *ptr++ = movt_immed_u16(addr, (uint16_t)(((uint32_t)offset >> 16) & 0xffff));
    } else if (offset == 0) {
        addr = base;                                   /* no displacement: addr IS base */
    } else if (!offset_32bit && offset > 0 && offset < 4096) {
        *ptr++ = add_immed(addr, base, (uint16_t)offset);
    } else if (!offset_32bit && offset < 0 && -offset < 4096) {
        *ptr++ = sub_immed(addr, base, (uint16_t)(-offset));
    } else {
        /* Large/32-bit displacement (base != 0xff): materialise it in x2 then add base.
         * base is never x2 here (the funnel passes an An/PC reg = w13..w18/w27..w29). */
        *ptr++ = movw_immed_u16(addr, (uint16_t)(offset & 0xffff));
        if ((uint32_t)offset >> 16) *ptr++ = movt_immed_u16(addr, (uint16_t)(((uint32_t)offset >> 16) & 0xffff));
        *ptr++ = add_reg(addr, base, addr, LSL, 0);
    }

    if (sz == 0 /* LEA: just leave the 68k address in val, no memory touch */) {
        if (addr != val) *ptr++ = mov_reg(val, addr);
        return ptr;
    }
    return ea_access(ptr, sz, is_store, is_signed, addr, val);
}

/* The INDEXED funnel (mode 6 = (d8,An,Xn), 7.3 = (d8,PC,Xn)). Mirrors load_reg_from_addr/
 * store_reg_to_addr: addr68k = base + (index << shift). The REAL decoder has already
 * sign/zero-extended the index register and selected the scale `shift` (0..3). */
uint32_t *j5d_ea_addr_index(uint32_t *ptr, unsigned kind, uint8_t base, uint8_t val,
                            uint8_t index, uint8_t shift)
{
    unsigned sz = kind & 3u, is_store = (kind >> 2) & 1u, is_signed = (kind >> 3) & 1u;

    /* index == 0xff means "no index register" — this funnel was used purely as a base+0
     * load (Emu68's load_reg_from_addr(size, base, reg, 0xff, ...)). Defer to the offset
     * form so (An)-with-no-index still routes through the sandbox. */
    if (index == 0xff)
        return j5d_ea_addr_offset(ptr, kind, base, val, 0, 0);

    if (base == 0xff) { *ptr++ = movw_immed_u16(2, 0); base = 2; }

    if (sz == 0 /* LEA-with-index: compute addr into val, no memory touch */) {
        *ptr++ = add_reg(val, base, index, LSL, shift);
        return ptr;
    }
    /* addr68k = base + (index << shift) -> x2 (32-bit 68k address). */
    *ptr++ = add_reg(2 /*x2*/, base, index, LSL, shift);
    return ea_access(ptr, sz, is_store, is_signed, 2, val);
}
