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
/* [J5m] bit6 = "load effective address" (lea/pea: compute the 68k addr into val, NO
 * memory touch, NO base-add, NO byteswap). This was PREVIOUSLY signalled by sz==0 — but
 * sz==0 ALSO means a BYTE-sized access (J5D_EA_SZ 0=B), so a real BYTE displacement/
 * indexed/abs store or load (Emu68 size==1) was MIS-decoded as LEA and skipped its memory
 * touch. The corpus only ever used the funnel for LONGWORD modes (bubsort's (d8,An,Xn.L)),
 * so the collision was latent; the C compiler's `move.b X,(d8,An,Xn)` / `move.b (An),Dn`
 * surfaced it. The LEA case now carries this EXPLICIT flag instead of overloading sz==0. */
#define J5D_EA_LEA(k)     (((k) >> 6) & 1u)

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
    unsigned is_lea = J5D_EA_LEA(kind);
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

    if (is_lea /* LEA: just leave the 68k address in val, no memory touch */) {
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
    unsigned is_lea = J5D_EA_LEA(kind);

    /* index == 0xff means "no index register" — this funnel was used purely as a base+0
     * load (Emu68's load_reg_from_addr(size, base, reg, 0xff, ...)). Defer to the offset
     * form so (An)-with-no-index still routes through the sandbox. */
    if (index == 0xff)
        return j5d_ea_addr_offset(ptr, kind, base, val, 0, 0);

    if (base == 0xff) { *ptr++ = movw_immed_u16(2, 0); base = 2; }

    if (is_lea /* LEA-with-index: compute addr into val, no memory touch */) {
        *ptr++ = add_reg(val, base, index, LSL, shift);
        return ptr;
    }
    /* addr68k = base + (index << shift) -> x2 (32-bit 68k address). */
    *ptr++ = add_reg(2 /*x2*/, base, index, LSL, shift);
    return ea_access(ptr, sz, is_store, is_signed, 2, val);
}

/* ============================ [J5l] the MOVEM sandbox EA ============================
 * movem (move-multiple-registers) is the opcode every compiler-generated 68k function uses
 * in its prologue/epilogue (`movem.l d2-d7/a2-a6,-(sp)` save + `movem.l (sp)+,d2-d7/a2-a6`
 * restore). We drive Emu68's REAL EMIT_MOVEM decoder (M68k_LINE4.c) — it does the whole
 * register-mask / predecrement-reversed-order / post-increment-An-update work — and route
 * ONLY its memory touches through the sandbox here, exactly as [J5d]/[J5g] did for the
 * (An)-class and displacement EAs.
 *
 * WHY movem needs its OWN helpers (it does NOT route through j5d_ea_mem / the funnel above).
 * EMIT_MOVEM does NOT use M68k_EA.c's `ldr_offset(reg_An,*arm_reg,imm)` sites that
 * emu68_darwinize.pl --ea-sandbox rewrites. Instead it resolves the EA base ONCE (via
 * EMIT_LoadFromEffectiveAddress with size 0 — so `base` holds the 68k ADDRESS, not a host
 * pointer) and then emits its OWN inner loop of raw, often PAIRED, sequential transfers
 * straight off `base`:
 *     stp/stp_preindex (a 32-bit W-register PAIR), str/str_offset_preindex, strh,
 *     ldr, ldp/ldp_postindex, ldrsh
 * On bare-metal Emu68 these work because An is a 1:1 host pointer and the CPU runs
 * big-endian (SCTLR.EE). On the little-endian hosted sandbox both are wrong — each access
 * needs host = base_adjust(x12) + (uint32_t)base (UXTW) and a per-register REV byteswap.
 * The A64.h stp/ldp here are the 32-bit pair forms (encoding 0x29.../0x28..., imm scaled /4),
 * so each .l element is a 32-bit value that must be byteswapped individually.
 *
 * emu68_darwinize.pl --movem-sandbox rewrites each of EMIT_MOVEM's memory encoder sites in
 * the BUILD-DIR COPY of M68k_LINE4.c to call one of these helpers (a pure call-substitution
 * preserving operand order). The QUARANTINE M68k_LINE4.c stays BYTE-VERBATIM. The DECODE
 * (mask order — REVERSED for predecrement, normal otherwise — block sizing, the base-on-the-
 * list `tmp_base_reg` save, the An post/pre update) is 100% the REAL decoder; only the memory
 * touch is sandbox-translated. The non-memory base arithmetic Emu68 emits (the `add_immed`/
 * `sub_immed` that bump/decrement the 68k An, and the `sub_immed(tmp_base_reg,...)` that
 * snapshots the base register's pre-decrement value) is left AS-IS — it operates on a 68k
 * address held in a register, no host translation needed.
 *
 * `base` is the AArch64 reg holding the 68k address; it is never x2/x3 (the RA hands out only
 * w4..w11, and EMIT_MOVEM maps An = w13..w29 or an alloc reg). `rt1`/`reg` are the 68k D/A
 * registers transferred (also never x2/x3). x3 = computed host pointer, x2 = REV scratch —
 * both RA-reserved, disjoint from the live file, mirroring j5d_ea_mem.
 *
 * IMPORTANT ORDERING NOTE (pre/post index): an AArch64 `stp_preindex(base,a,b,-N)` updates
 *   base BEFORE the access (`base-=N; [base]=a; [base+4]=b`); `ldp_postindex(base,a,b,N)`
 *   updates base AFTER (`a=[base]; b=[base+4]; base+=N`). We reproduce that exactly: pre-index
 *   adjusts `base` first, post-index last. The 68k An-update semantics this realises
 *   (-(An) decrements before store, (An)+ increments after load) are what the decoder asked
 *   for. (For movem, EMIT_MOVEM also emits a separate `add_immed(base,base,block_size)` to
 *   bump An after a NON-pair-fast-path post-increment load — that is non-memory and untouched.)
 * ===================================================================================*/

/* Emit `host = base_adjust(x12) + (uint32_t)base68k` -> x3, with the +offset folded into the
 * subsequent load/store (the A64 ldr/str offset field). For offsets the small immediate
 * fits the scaled offset field; movem offsets are small (<= 15 regs * 4 = 60). */
static uint32_t *movem_hostptr(uint32_t *ptr, uint8_t base68k)
{
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, base68k, UXTW, 0);
    return ptr;
}

/* One sandbox WORD store: mem68k[base+byte_off] = val (REV to big-endian). */
static uint32_t *movem_store_w(uint32_t *ptr, uint8_t base68k, uint8_t val, int byte_off)
{
    g_j5d_ea_emits++;
    ptr = movem_hostptr(ptr, base68k);
    *ptr++ = rev(2 /*w2*/, val);
    *ptr++ = str_offset(J5D_HOSTPTR, 2, (uint16_t)byte_off);
    return ptr;
}
/* One sandbox HALF store: mem68k[base+byte_off] = (uint16)val (REV16 to big-endian). */
static uint32_t *movem_store_h(uint32_t *ptr, uint8_t base68k, uint8_t val, int byte_off)
{
    g_j5d_ea_emits++;
    ptr = movem_hostptr(ptr, base68k);
    *ptr++ = rev16(2 /*w2*/, val);
    *ptr++ = strh_offset(J5D_HOSTPTR, 2, (uint16_t)byte_off);
    return ptr;
}
/* One sandbox WORD load: val = mem68k[base+byte_off] (REV from big-endian). */
static uint32_t *movem_load_w(uint32_t *ptr, uint8_t base68k, uint8_t val, int byte_off)
{
    g_j5d_ea_emits++;
    ptr = movem_hostptr(ptr, base68k);
    *ptr++ = ldr_offset(J5D_HOSTPTR, val, (uint16_t)byte_off);
    *ptr++ = rev(val, val);
    return ptr;
}
/* One sandbox signed-HALF load: val = (int16)mem68k[base+byte_off], sign-extended to 32 bits
 * (movem .w loads sign-extend word->long into the destination register — M68000 PRM). */
static uint32_t *movem_load_sh(uint32_t *ptr, uint8_t base68k, uint8_t val, int byte_off)
{
    g_j5d_ea_emits++;
    ptr = movem_hostptr(ptr, base68k);
    *ptr++ = ldrh_offset(J5D_HOSTPTR, val, (uint16_t)byte_off);
    *ptr++ = rev16(val, val);
    *ptr++ = sxth(val, val);
    return ptr;
}

/* ---- the encoder-shaped entry points the darwinize --movem-sandbox rewrite calls.
 * Each mirrors ONE A64.h encoder EMIT_MOVEM emitted; the pre/post-index ones also update
 * `base` (the 68k An address reg) by `amt` (= block_size, always positive here). ---- */

/* was stp(base, rt1, rt2, offset)            (32-bit W-pair store, no index) */
uint32_t *j5d_movem_stp(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int offset)
{
    ptr = movem_store_w(ptr, base, rt1, offset);
    ptr = movem_store_w(ptr, base, rt2, offset + 4);
    return ptr;
}
/* was stp_preindex(base, rt1, rt2, -amt)     (base-=amt FIRST, then the W-pair store) */
uint32_t *j5d_movem_stp_pre(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int amt)
{
    *ptr++ = sub_immed(base, base, (uint16_t)amt);     /* pre-decrement the 68k An */
    ptr = movem_store_w(ptr, base, rt1, 0);
    ptr = movem_store_w(ptr, base, rt2, 4);
    return ptr;
}
/* was str_offset(base, rt, offset)           (single 32-bit store, no index) */
uint32_t *j5d_movem_str(uint32_t *ptr, uint8_t base, uint8_t rt, int offset)
{
    return movem_store_w(ptr, base, rt, offset);
}
/* was str_offset_preindex(base, rt, -amt)    (base-=amt FIRST, then the single store) */
uint32_t *j5d_movem_str_pre(uint32_t *ptr, uint8_t base, uint8_t rt, int amt)
{
    *ptr++ = sub_immed(base, base, (uint16_t)amt);
    return movem_store_w(ptr, base, rt, 0);
}
/* was strh_offset(base, rt, offset)          (single 16-bit store, no index) */
uint32_t *j5d_movem_strh(uint32_t *ptr, uint8_t base, uint8_t rt, int offset)
{
    return movem_store_h(ptr, base, rt, offset);
}
/* was strh_offset_preindex(base, rt, -amt)   (base-=amt FIRST, then the 16-bit store) */
uint32_t *j5d_movem_strh_pre(uint32_t *ptr, uint8_t base, uint8_t rt, int amt)
{
    *ptr++ = sub_immed(base, base, (uint16_t)amt);
    return movem_store_h(ptr, base, rt, 0);
}
/* was ldp(base, rt1, rt2, offset)            (32-bit W-pair load, no index) */
uint32_t *j5d_movem_ldp(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int offset)
{
    ptr = movem_load_w(ptr, base, rt1, offset);
    ptr = movem_load_w(ptr, base, rt2, offset + 4);
    return ptr;
}
/* was ldp_postindex(base, rt1, rt2, amt)     (W-pair load FIRST, then base+=amt)
 * EMIT_MOVEM only emits ldp_postindex with amt=8 (its block_size==8 two-longword fast path,
 * M68k_LINE4.c:2636-2637), and that path is taken only when neither loaded reg is the base An
 * (the base is skipped in post-increment mode), so rt1/rt2 never alias `base`. We honour the
 * literal amt the decoder passed; if that gate ever loosens, amt still tracks the decoder. */
uint32_t *j5d_movem_ldp_post(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int amt)
{
    ptr = movem_load_w(ptr, base, rt1, 0);
    ptr = movem_load_w(ptr, base, rt2, 4);
    *ptr++ = add_immed(base, base, (uint16_t)amt);     /* post-increment the 68k An */
    return ptr;
}
/* was ldr_offset(base, rt, offset)           (single 32-bit load, no index) */
uint32_t *j5d_movem_ldr(uint32_t *ptr, uint8_t base, uint8_t rt, int offset)
{
    return movem_load_w(ptr, base, rt, offset);
}
/* was ldr_offset_postindex(base, rt, amt)    (single 32-bit load, THEN base += amt)
 * [J5m]: the (An)+ pop EMIT_UNLK emits — load the longword at (An), byteswap, then
 * post-increment the 68k An. Used by the --movem-sandbox unlk rule. */
uint32_t *j5d_movem_ldr_post(uint32_t *ptr, uint8_t base, uint8_t rt, int amt)
{
    ptr = movem_load_w(ptr, base, rt, 0);
    *ptr++ = add_immed(base, base, (uint16_t)amt);     /* post-increment the 68k An */
    return ptr;
}
/* was ldrsh_offset(base, rt, offset)         (single signed-16 load, sign-extend to long) */
uint32_t *j5d_movem_ldrsh(uint32_t *ptr, uint8_t base, uint8_t rt, int offset)
{
    return movem_load_sh(ptr, base, rt, offset);
}

/* ============================ [J5o] the FP-MEMORY sandbox EA =======================
 * The 68881/68882 FMOVE format-conversion to/from MEMORY (FMOVE.d (An),FPn / FMOVE.s ... /
 * etc). We DRIVE Emu68's REAL EMIT_FPU / FPU_FetchData / FPU_StoreData decoders — they do
 * the whole FPU-opcode decode, the format selection, the EA-base resolve, and the AArch64
 * fp arithmetic. Only their final MEMORY TOUCH is sandbox-translated here, exactly as
 * [J5l] did for movem and [J5d]/[J5g] for the integer EA.
 *
 * WHY FP needs its OWN helpers (it does NOT route through j5d_ea_mem / the funnel / movem).
 * FPU_FetchData/FPU_StoreData resolve the EA base ONCE (EMIT_LoadFromEffectiveAddress with
 * size 0 -> `int_reg` / `off` holds the 68k ADDRESS, not a host pointer) and then emit their
 * OWN fp load/store straight off that address:
 *     *ptr++ = fldd(*reg, int_reg, imm_offset);            // FMOVE.d (mem) -> FPn
 *     *ptr++ = fldd_preindex/postindex(*reg, int_reg, sz); // -(An)/(An)+
 *     *ptr++ = flds(*reg, int_reg, imm_offset);            // FMOVE.s (mem) -> FPn (then fcvtds)
 *     *ptr++ = fstd(reg, int_reg, imm_offset);             // FPn -> FMOVE.d (mem)
 *     *ptr++ = fsts(vfp_reg, int_reg, imm_offset);         // FPn -> FMOVE.s (mem) (after fcvtsd)
 * On bare-metal Emu68 these work because An is a 1:1 host pointer and the CPU runs big-endian
 * (SCTLR.EE). On the little-endian hosted sandbox both are wrong: each FP element needs
 * host = base_adjust(x12) + (uint32_t)base (UXTW) and a per-element byteswap (REV64 for the
 * 8-byte double, REV for the 4-byte single — IEEE doubles/singles are byte-reversed across
 * endianness exactly like integers). emu68_darwinize.pl --fpu-sandbox rewrites each such site
 * in the BUILD-DIR COPY of M68k_LINEF.c to call one of these helpers; the QUARANTINE
 * M68k_LINEF.c stays BYTE-VERBATIM. The DECODE + the fp ARITHMETIC stay 100% the REAL decoder.
 *
 * PRECISION: the value crossing memory is the 68k operand's NATIVE width — .d is the IEEE
 * binary64 the FP file already holds (no conversion at the memory boundary, just byteswap);
 * .s is IEEE binary32 in memory, widened to the d-reg's binary64 by fcvtds on load (the REAL
 * decoder emits the fcvtds AFTER our flds) / narrowed by fcvtsd before our fsts on store. The
 * .x (80-bit extended) / .p (packed) memory forms are DEFERRED (the precision model is double;
 * 80-bit exactness is not bit-reproducible on AArch64) — the decoder's .x/.p paths call the
 * Load96bit/PackedToDouble runtime helpers, stubbed in j5o_fpu_shims.c and not exercised here.
 *
 * Scratch: x3 = host pointer, x2 = GP byteswap value (both RA-reserved, disjoint from the live
 * 68k file w13..w29 / REG_PC w18 / RA pool w4..w11 / x12 base-adjust — same as the movem/EA
 * helpers). The FP scratch d-reg for the single conversion is d1 (an AArch64 temporary the FP
 * file d8..d15 and the d2..d7 scratch pool never alias; the REAL decoder's vfp_reg for stores
 * is a d2..d7 alloc, passed to us). `base68k` is an An/PC/alloc GP reg, never x2/x3. ===========*/

/* Double (.d) load: d[fpreg] = byteswap64(mem68k[base+offset]). */
static uint32_t *fpu_load_d(uint32_t *ptr, uint8_t fpreg, uint8_t base68k, int offset)
{
    g_j5d_ea_emits++;
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, base68k, UXTW, 0);
    if (offset >= 0)      *ptr++ = ldur64_offset(J5D_HOSTPTR, 2 /*x2*/, (int16_t)offset);
    else                  *ptr++ = ldur64_offset(J5D_HOSTPTR, 2 /*x2*/, (int16_t)offset);
    *ptr++ = rev64(2, 2);
    *ptr++ = mov_reg_to_simd(fpreg, TS_D, 0, 2);   /* fmov d[fpreg], x2 */
    return ptr;
}
/* Double (.d) store: mem68k[base+offset] = byteswap64(d[fpreg]). */
static uint32_t *fpu_store_d(uint32_t *ptr, uint8_t fpreg, uint8_t base68k, int offset)
{
    g_j5d_ea_emits++;
    *ptr++ = mov_simd_to_reg(2 /*x2*/, fpreg, TS_D, 0);   /* fmov x2, d[fpreg] */
    *ptr++ = rev64(2, 2);
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, base68k, UXTW, 0);
    *ptr++ = stur64_offset(J5D_HOSTPTR, 2, (int16_t)offset);
    return ptr;
}
/* Single (.s) load into the d-reg: load 4 bytes, byteswap, fmsr to the single half, then the
 * REAL decoder emits fcvtds(*reg,*reg) after us to widen to double. We write the single half. */
static uint32_t *fpu_load_s(uint32_t *ptr, uint8_t fpreg, uint8_t base68k, int offset)
{
    g_j5d_ea_emits++;
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, base68k, UXTW, 0);
    *ptr++ = ldr_offset(J5D_HOSTPTR, 2 /*w2*/, (uint16_t)(offset >= 0 ? offset : 0));
    *ptr++ = rev(2, 2);
    *ptr++ = fmsr(fpreg, 2);                       /* d[fpreg].s = w2 (single half) */
    return ptr;
}
/* Single (.s) store: the REAL decoder already emitted fcvtsd(vfp_reg, reg) before calling us,
 * so vfp_reg holds the binary32 in its single half — read it, byteswap, store 4 bytes. */
static uint32_t *fpu_store_s(uint32_t *ptr, uint8_t vfp_reg, uint8_t base68k, int offset)
{
    g_j5d_ea_emits++;
    *ptr++ = fmrs(2 /*w2*/, vfp_reg);              /* w2 = vfp_reg.s */
    *ptr++ = rev(2, 2);
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, base68k, UXTW, 0);
    *ptr++ = str_offset(J5D_HOSTPTR, 2, (uint16_t)(offset >= 0 ? offset : 0));
    return ptr;
}

/* ---- encoder-shaped entry points the darwinize --fpu-sandbox rewrite calls. Each mirrors
 * ONE A64.h FP encoder; the pre/post-index ones also update `base` (the 68k An address) by
 * the element size (8 for .d, 4 for .s). The decoder passes the SAME operands the original
 * encoder used; the index amount the decoder computed (pre_sz<0 / post_sz>0) is honoured. ---- */

/* was fldd(reg, base, offset)               (.d load, plain offset)            */
uint32_t *j5d_fpu_fldd(uint32_t *ptr, uint8_t reg, uint8_t base, int offset)
{ return fpu_load_d(ptr, reg, base, offset); }
/* was fldd_preindex(reg, base, pre_sz<0)    (base += pre_sz FIRST, then .d load) */
uint32_t *j5d_fpu_fldd_pre(uint32_t *ptr, uint8_t reg, uint8_t base, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_load_d(ptr, reg, base, 0);
}
/* was fldd_postindex(reg, base, post_sz>0)  (.d load, THEN base += post_sz)      */
uint32_t *j5d_fpu_fldd_post(uint32_t *ptr, uint8_t reg, uint8_t base, int post_sz)
{
    ptr = fpu_load_d(ptr, reg, base, 0);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
/* was fldd_pimm(reg, base, offset>>3)       (.d load, unsigned scaled offset)    */
uint32_t *j5d_fpu_fldd_pimm(uint32_t *ptr, uint8_t reg, uint8_t base, int offset_div8)
{ return fpu_load_d(ptr, reg, base, offset_div8 * 8); }

/* was fstd(reg, base, offset)               (.d store, plain offset)            */
uint32_t *j5d_fpu_fstd(uint32_t *ptr, uint8_t reg, uint8_t base, int offset)
{ return fpu_store_d(ptr, reg, base, offset); }
uint32_t *j5d_fpu_fstd_pre(uint32_t *ptr, uint8_t reg, uint8_t base, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_store_d(ptr, reg, base, 0);
}
uint32_t *j5d_fpu_fstd_post(uint32_t *ptr, uint8_t reg, uint8_t base, int post_sz)
{
    ptr = fpu_store_d(ptr, reg, base, 0);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
uint32_t *j5d_fpu_fstd_pimm(uint32_t *ptr, uint8_t reg, uint8_t base, int offset_div8)
{ return fpu_store_d(ptr, reg, base, offset_div8 * 8); }

/* was flds(reg, base, offset)               (.s load into single half)         */
uint32_t *j5d_fpu_flds(uint32_t *ptr, uint8_t reg, uint8_t base, int offset)
{ return fpu_load_s(ptr, reg, base, offset); }
uint32_t *j5d_fpu_flds_pre(uint32_t *ptr, uint8_t reg, uint8_t base, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_load_s(ptr, reg, base, 0);
}
uint32_t *j5d_fpu_flds_post(uint32_t *ptr, uint8_t reg, uint8_t base, int post_sz)
{
    ptr = fpu_load_s(ptr, reg, base, 0);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
uint32_t *j5d_fpu_flds_pimm(uint32_t *ptr, uint8_t reg, uint8_t base, int offset_div4)
{ return fpu_load_s(ptr, reg, base, offset_div4 * 4); }

/* was fsts(vfp_reg, base, offset)           (.s store from single half)        */
uint32_t *j5d_fpu_fsts(uint32_t *ptr, uint8_t reg, uint8_t base, int offset)
{ return fpu_store_s(ptr, reg, base, offset); }
uint32_t *j5d_fpu_fsts_pre(uint32_t *ptr, uint8_t reg, uint8_t base, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_store_s(ptr, reg, base, 0);
}
uint32_t *j5d_fpu_fsts_post(uint32_t *ptr, uint8_t reg, uint8_t base, int post_sz)
{
    ptr = fpu_store_s(ptr, reg, base, 0);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
uint32_t *j5d_fpu_fsts_pimm(uint32_t *ptr, uint8_t reg, uint8_t base, int offset_div4)
{ return fpu_store_s(ptr, reg, base, offset_div4 * 4); }

/* ===================== [J5o] FP INTEGER-FORMAT (.l/.w/.b) MEMORY <-> reg ====================
 * FMOVE with an INTEGER source/destination memory format (.l/.w/.b) does NOT use the fldd/flds
 * FP load/store: the REAL decoder converts in a GP reg (fcvtzs/scvtf via val_reg) and emits a
 * plain INTEGER ldr/str (+h/+b, +ur/pre/post) straight off int_reg/off — the same 1:1-MMU +
 * big-endian assumption the (An) integer EA had, but emitted INLINE in M68k_LINEF.c (not via the
 * darwinized M68k_EA.c). So these sites need the SAME sandbox base-adjust + byteswap as j5d_ea_mem.
 * The darwinize --fpu-sandbox rewrite routes each such site (base operand = int_reg / off) here.
 *
 *   LOAD (mem -> val_reg, then the decoder does scvtf_32toD(*reg,val_reg)):
 *     .l : ldur w_val,[x3]      ; rev   w_val           (32-bit, signed by scvtf_32toD)
 *     .w : ldurh w_val,[x3]     ; rev16 w_val ; sxth    (the decoder used ldrsh -> sign-extend)
 *     .b : ldurb w_val,[x3]     ; sxtb                  (no byteswap; decoder used ldrsb)
 *   STORE (val_reg already holds the saturated/converted int, host order):
 *     .l : rev   w2,w_val ; stur  w2,[x3]
 *     .w : rev16 w2,w_val ; sturh w2,[x3]
 *     .b :                  sturb w_val,[x3]
 * x3 = host pointer (= x12 base-adjust + (uint32_t)base, UXTW); x2 = REV scratch — both
 * RA-reserved, disjoint from the live 68k file / REG_PC / the RA pool / the FP file, exactly as
 * j5d_ea_mem + the FP single/double helpers. `base` is int_reg/off (An/PC/alloc GP); never x2/x3.
 * Sizes: 4 (.l), 2 (.w), 1 (.b). pre/post variants update `base` (the 68k address) by the element
 * size, as the original *_preindex / *_postindex encoders did. ----------------------------------*/

/* compute x3 = base_adjust + (uint32_t)base68k, with the +offset folded by the access's signed-9. */
static uint32_t *fpu_int_addr(uint32_t *ptr, uint8_t base68k)
{
    *ptr++ = add64_reg_ext(J5D_HOSTPTR, J5D_BASEADJ, base68k, UXTW, 0);
    return ptr;
}
/* integer LOAD of size sz (4/2/1) at base68k+offset into val_reg (sign-correct for scvtf). */
static uint32_t *fpu_int_load(uint32_t *ptr, uint8_t val, uint8_t base68k, int offset, int sz)
{
    g_j5d_ea_emits++;
    ptr = fpu_int_addr(ptr, base68k);
    if (sz == 4) {
        *ptr++ = ldur_offset(J5D_HOSTPTR, val, (int16_t)offset);
        *ptr++ = rev(val, val);
    } else if (sz == 2) {
        *ptr++ = ldursh_offset(J5D_HOSTPTR, val, (int16_t)offset); /* sign-ext halfword load */
        *ptr++ = rev16(val, val);                                  /* byteswap the 2 low bytes */
        *ptr++ = sxth(val, val);                                   /* re-sign-extend post-swap */
    } else { /* sz == 1 */
        *ptr++ = ldursb_offset(J5D_HOSTPTR, val, (int16_t)offset); /* sign-ext byte; no swap   */
    }
    return ptr;
}
/* integer STORE of size sz (4/2/1) of val_reg (host order) to base68k+offset (byteswapped). */
static uint32_t *fpu_int_store(uint32_t *ptr, uint8_t val, uint8_t base68k, int offset, int sz)
{
    g_j5d_ea_emits++;
    ptr = fpu_int_addr(ptr, base68k);
    if (sz == 4) {
        *ptr++ = rev(2 /*w2 scratch*/, val);
        *ptr++ = stur_offset(J5D_HOSTPTR, 2, (int16_t)offset);
    } else if (sz == 2) {
        *ptr++ = rev16(2, val);
        *ptr++ = sturh_offset(J5D_HOSTPTR, 2, (int16_t)offset);
    } else { /* sz == 1 */
        *ptr++ = sturb_offset(J5D_HOSTPTR, val, (int16_t)offset);
    }
    return ptr;
}

/* ---- encoder-shaped entry points the darwinize --fpu-sandbox rewrite calls (integer format).
 * Each mirrors ONE A64.h integer load/store encoder by NAME; the pre/post-index ones update the
 * 68k base address (sub before / add after) by the element size, exactly as the *_preindex /
 * *_postindex encoders they replace. The decoder passes (base, val, offset) just as it did to the
 * original encoder; index amounts come from the decoder's pre_sz<0 / post_sz>0. ----------------*/

/* --- .l (longword, sz=4) --- */
/* was ldr_offset(int_reg, val_reg, off) / ldur_offset(...) — plain offset .l load */
uint32_t *j5d_fpu_ildr(uint32_t *ptr, uint8_t base, uint8_t val, int off)
{ return fpu_int_load(ptr, val, base, off, 4); }
uint32_t *j5d_fpu_ildr_pre(uint32_t *ptr, uint8_t base, uint8_t val, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_int_load(ptr, val, base, 0, 4);
}
uint32_t *j5d_fpu_ildr_post(uint32_t *ptr, uint8_t base, uint8_t val, int post_sz)
{
    ptr = fpu_int_load(ptr, val, base, 0, 4);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
/* was str_offset / stur_offset — plain offset .l store */
uint32_t *j5d_fpu_istr(uint32_t *ptr, uint8_t base, uint8_t val, int off)
{ return fpu_int_store(ptr, val, base, off, 4); }
uint32_t *j5d_fpu_istr_pre(uint32_t *ptr, uint8_t base, uint8_t val, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_int_store(ptr, val, base, 0, 4);
}
uint32_t *j5d_fpu_istr_post(uint32_t *ptr, uint8_t base, uint8_t val, int post_sz)
{
    ptr = fpu_int_store(ptr, val, base, 0, 4);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}

/* --- .w (word/halfword, sz=2) --- */
/* was ldrsh_offset / ldursh_offset — sign-extending .w load */
uint32_t *j5d_fpu_ildrh(uint32_t *ptr, uint8_t base, uint8_t val, int off)
{ return fpu_int_load(ptr, val, base, off, 2); }
uint32_t *j5d_fpu_ildrh_pre(uint32_t *ptr, uint8_t base, uint8_t val, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_int_load(ptr, val, base, 0, 2);
}
uint32_t *j5d_fpu_ildrh_post(uint32_t *ptr, uint8_t base, uint8_t val, int post_sz)
{
    ptr = fpu_int_load(ptr, val, base, 0, 2);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
/* was strh_offset / sturh_offset — .w store */
uint32_t *j5d_fpu_istrh(uint32_t *ptr, uint8_t base, uint8_t val, int off)
{ return fpu_int_store(ptr, val, base, off, 2); }
uint32_t *j5d_fpu_istrh_pre(uint32_t *ptr, uint8_t base, uint8_t val, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_int_store(ptr, val, base, 0, 2);
}
uint32_t *j5d_fpu_istrh_post(uint32_t *ptr, uint8_t base, uint8_t val, int post_sz)
{
    ptr = fpu_int_store(ptr, val, base, 0, 2);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}

/* --- .b (byte, sz=1) --- */
/* was ldrsb_offset / ldursb_offset — sign-extending .b load */
uint32_t *j5d_fpu_ildrb(uint32_t *ptr, uint8_t base, uint8_t val, int off)
{ return fpu_int_load(ptr, val, base, off, 1); }
uint32_t *j5d_fpu_ildrb_pre(uint32_t *ptr, uint8_t base, uint8_t val, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_int_load(ptr, val, base, 0, 1);
}
uint32_t *j5d_fpu_ildrb_post(uint32_t *ptr, uint8_t base, uint8_t val, int post_sz)
{
    ptr = fpu_int_load(ptr, val, base, 0, 1);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
/* was strb_offset / sturb_offset — .b store */
uint32_t *j5d_fpu_istrb(uint32_t *ptr, uint8_t base, uint8_t val, int off)
{ return fpu_int_store(ptr, val, base, off, 1); }
uint32_t *j5d_fpu_istrb_pre(uint32_t *ptr, uint8_t base, uint8_t val, int pre_sz)
{
    if (pre_sz < 0) *ptr++ = sub_immed(base, base, (uint16_t)(-pre_sz));
    else            *ptr++ = add_immed(base, base, (uint16_t)pre_sz);
    return fpu_int_store(ptr, val, base, 0, 1);
}
uint32_t *j5d_fpu_istrb_post(uint32_t *ptr, uint8_t base, uint8_t val, int post_sz)
{
    ptr = fpu_int_store(ptr, val, base, 0, 1);
    *ptr++ = add_immed(base, base, (uint16_t)post_sz);
    return ptr;
}
