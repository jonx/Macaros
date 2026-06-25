/* j5a_build.c — [J5a] run path: translate a memory-touching block via the ADOPTED
 * Emu68 EMITTER, with a HAND-ROLLED EA decode + the sandbox-pointer boundary (GLUE,
 * OURS, AROS-licensed).
 *
 * This extends the [J2]/[J4] translate->emit->run pipeline to memory load/store. It
 * hand-decodes the small block (read from the RELOCATED sandbox) and turns each
 * opcode into AArch64 by CALLING the adopted Emu68 encoders (the verbatim,
 * MPL-quarantined emu68/A64.h), writes the words into the [J1] MAP_JIT region via the
 * jit_region API, and runs the block under W^X.
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS
 * (AROS-licensed). It #includes the quarantined Emu68 emitter and CALLS its inline
 * encoders; per MPL-2.0 / Mozilla FAQ Q11-Q13 that does NOT relicense this file. No
 * Emu68 source is copied here. Only encoder functions are invoked; NONE of Emu68's
 * runtime (RA_*, M68k_EA.c, cache.c, M68k_Translator, MainLoop) is linked. See the
 * [J5a] adoption finding in j5a_jit68k.h: the EA decode + register allocator do NOT
 * lift incrementally (An-is-host-pointer 1:1 MMU model, the ICACHE software cache,
 * and the EL0-system-register SR/CTX model), so the EA/memory path here is OURS,
 * built around the emitter that DID lift cleanly.
 *
 * ============================ THE SANDBOX-POINTER BOUNDARY =====================
 * The 68k program addresses a 32-bit big-endian sandbox. A memory op through An maps
 * the 68k (sandbox) address to a host pointer:  host = host_mem + (An - origin).
 * We hand the block a SINGLE adjusted base  x1 = (host_mem - origin)  so each access
 * is  host = x1 + An  (zero-extending the 32-bit An into the 64-bit add via UXTW).
 * Around every access the block BOUNDS-CHECKS An: the single unsigned compare
 *   (An - origin) >u (size - 4)
 * catches both An < origin and An + 4 > origin + size. On a fault the block sets
 * state->fault = J5A_FAULT_OOB and SKIPS the access (it never dereferences out of the
 * sandbox — no host OOB). Loads/stores are BIG-ENDIAN (68k): the block byteswaps the
 * longword with REV around the host load/store.
 * ===============================================================================
 *
 * ============================= SCOPE / DEFERRALS ===============================
 * Opcodes hand-decoded here (the [J5a] memory increment + carried-forward regs):
 *   moveq #imm8,Dn  (imm8 < 0x80 — the [J2] zero/sign-extend shortcut, enforced)
 *   addq.l #imm,Dn  (imm in 1..8)
 *   add.l  Dm,Dn    (register-direct .L)
 *   move.l (An),Dn  (BE load  from the sandbox via An)   <-- [J5a] new
 *   move.l Dn,(An)  (BE store to  the sandbox via An)    <-- [J5a] new
 *   rts
 * DEFERRED to [J5b] and beyond: branches/loops, full opcode + addressing-mode
 * coverage, our own register allocator around the emitter, real jsr-through-vector
 * decode from a stream, library calls FROM the running program, and a sandbox-backed
 * allocator for return-pointers outside the sandbox. [J5a] is load/store + the
 * sandbox-pointer boundary only.
 * ===============================================================================
 */
#include "j5a_jit68k.h"
#include "jit_region.h"
#include "emu68/A64.h"      /* adopted MPL emitter — encoders only, called not copied */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* The single external symbol A64.h's ASSERT_REG macro references (never called for
 * valid register numbers — we only pass registers < 32). No-op satisfies the link. */
__attribute__((weak)) void kprintf(const char *format, ...) { (void)format; }

static jit_region g_region;
static int        g_region_live = 0;

/* Big-endian 16-bit fetch from the 68k stream (the sandbox is big-endian). */
static uint16_t fetch_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* ---- AArch64 scratch register choice ------------------------------------------
 * Emu68's m68k map: D0..D7 -> w19..w26, A0..A7 -> w13..w20 (REG_A0=13). The block is
 * entered as a C function pointer:  void block(state*, uint64_t base_adj);  so x0 is
 * the state pointer and x1 is the adjusted sandbox base (host_mem - origin). We use
 * x9..x12 as caller-clobberable scratch — disjoint from the 68k register map and from
 * x0/x1 — for the host-pointer computation, the bounds compare, and the byteswap.
 * (Emu68's own RA hands out x0..x11; we avoid x0/x1 since they carry our two args and
 * stay live across the whole block.) */
#define SCR_BASE   1u    /* x1: adjusted sandbox base (host_mem - origin)         */
#define SCR_ADDR   9u    /* x9: computed host pointer / An working copy           */
#define SCR_TMP   10u    /* x10: bounds-limit / byteswap scratch                  */
#define SCR_VAL   11u    /* x11: the longword being loaded/stored (host order)    */

static const uint8_t reg_d[8] = { REG_D0, REG_D1, REG_D2, REG_D3,
                                  REG_D4, REG_D5, REG_D6, REG_D7 };
static const uint8_t reg_a[8] = { REG_A0, REG_A1, REG_A2, REG_A3,
                                  REG_A4, REG_A5, REG_A6, REG_A7 };

/* Materialise a 32-bit immediate into wreg via mov/movk (handles the full range, so
 * `origin` and `size` of any value work). */
static unsigned emit_mov_u32(uint32_t *out, unsigned n, uint8_t wreg, uint32_t imm)
{
    out[n++] = mov_immed_u16(wreg, (uint16_t)(imm & 0xFFFFu), 0);
    if (imm >> 16)
        out[n++] = movk_immed_u16(wreg, (uint16_t)(imm >> 16), 1);
    return n;
}

/* Emit the sandbox bounds check + host-pointer computation for the An in `an_wreg`.
 * Produces:
 *     w_addr := An                       (32-bit working copy)
 *     w_tmp  := An - origin              (offset into sandbox, 32-bit, may wrap)
 *     cmp w_tmp, (size-4)                (unsigned)
 *     b.ls  ok                           (in-range:  (An-origin) <= size-4)
 *     // fault path: state->fault = J5A_FAULT_OOB ; branch to epilogue (skip access)
 *     w_tmp := J5A_FAULT_OOB ; str w_tmp,[x0,#fault] ; b -> fault_target
 *   ok:
 *     x_addr := x1 + (An UXTW)           (the host pointer = base_adj + An)
 * `fault_rel_words` is the forward distance (in 32-bit words, from the fault branch)
 * to the shared fault-exit landing pad. Returns the new word count. The host pointer
 * lands in x_addr (== SCR_ADDR). */
static unsigned emit_ea_indirect(uint32_t *out, unsigned n, uint8_t an_wreg,
                                 uint32_t origin, uint32_t size,
                                 int *fault_branch_idx)
{
    /* w_addr := An */
    out[n++] = mov_reg(SCR_ADDR, an_wreg);

    /* w_tmp := An - origin  (subtract origin to get the sandbox offset).
     * sub w_tmp, w_addr, #origin — origin may exceed imm12, so materialise+sub_reg. */
    n = emit_mov_u32(out, n, SCR_TMP, origin);
    out[n++] = sub_reg(SCR_TMP, SCR_ADDR, SCR_TMP, LSL, 0);   /* w_tmp = An - origin */

    /* cmp w_tmp, #(size-4) — unsigned: in-range iff (An-origin) <= size-4. */
    {
        uint32_t limit = (size >= 4) ? (size - 4u) : 0u;
        n = emit_mov_u32(out, n, SCR_VAL, limit);             /* reuse SCR_VAL as limit */
        out[n++] = cmp_reg(SCR_TMP, SCR_VAL, LSL, 0);         /* sets NZCV from w_tmp-limit */
    }

    /* b.ls ok  (LS = unsigned lower-or-same -> in range). It must skip exactly the
     * fault-set sequence below (which is variable length: emit_mov_u32 is 1 or 2
     * insns). We emit a placeholder now and patch the real forward distance once the
     * fault block + the host-ptr add are placed, so the skip is always exact. */
    int bcc_idx = (int)n;
    out[n++] = 0;                   /* placeholder for `b.ls ok` (patched below) */

    /* fault path: state->fault = J5A_FAULT_OOB; then branch to the shared epilogue. */
    n = emit_mov_u32(out, n, SCR_TMP, J5A_FAULT_OOB);
    out[n++] = str_offset(0 /*x0 state*/, SCR_TMP, J5A_OFF_FAULT);
    *fault_branch_idx = (int)n;     /* caller patches this `b` to the fault-exit pad */
    out[n++] = 0;                   /* placeholder for `b fault_exit` (patched later) */

    /* ok: x_addr := x1 + (An, UXTW). 64-bit add, zero-extending the 32-bit An. */
    int ok_idx = (int)n;
    out[n++] = add64_reg_ext(SCR_ADDR, SCR_BASE, an_wreg, UXTW, 0);

    /* Patch the conditional skip to land exactly on the host-ptr add (the `ok` label). */
    out[bcc_idx] = b_cc(9 /*LS = unsigned lower-or-same*/, (int32_t)(ok_idx - bcc_idx));

    return n;
}

/* Decode the small block from `code` (the relocated sandbox bytes, `code_len` bytes)
 * and emit the equivalent AArch64 into `out`. Returns the number of 32-bit words on
 * success, or 0 on a decode/emit error (errbuf set). `origin`/`size` describe the
 * sandbox for the bounds check. The negative-control flags alter the emit. */
static unsigned emit_block(const uint8_t *code, uint32_t code_len,
                           uint32_t origin, uint32_t size,
                           int neg_endianness, int neg_skip_store,
                           uint32_t *out, char *errbuf, unsigned errlen)
{
#define EFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 0; } while (0)

    unsigned n = 0;

    /* AAPCS64 PRESERVE (the [J4] fix): D0..D7=x19..x26 and A0..A7=x13..x20 overlap
     * AArch64 callee-saved x19..x28 — save/restore them so the block is a valid C
     * function pointer. (REG_A0=13..REG_A7=20: x19,x20 are saved by the x19/x20 pair.) */
    out[n++] = stp64_preindex(31, 29, 30, -16);
    out[n++] = stp64_preindex(31, 19, 20, -16);
    out[n++] = stp64_preindex(31, 21, 22, -16);
    out[n++] = stp64_preindex(31, 23, 24, -16);
    out[n++] = stp64_preindex(31, 25, 26, -16);
    out[n++] = stp64_preindex(31, 27, 28, -16);

    /* Prologue: load D0..D7 and A0..A7 from the state struct (x0). */
    for (int i = 0; i < 8; i++) out[n++] = ldr_offset(0, reg_d[i], J5A_OFF_D(i));
    for (int i = 0; i < 8; i++) out[n++] = ldr_offset(0, reg_a[i], J5A_OFF_A(i));

    /* Collect fault-branch placeholder indices to patch to the shared fault pad. */
    int fault_idx[16];
    int n_fault = 0;

    uint32_t ip = 0;
    int saw_rts = 0;
    while (ip + 2 <= code_len) {
        uint16_t op = fetch_be16(code + ip);
        ip += 2;

        if ((op & 0xF100u) == 0x7000u) {            /* moveq #imm8,Dn */
            unsigned dn  = (op >> 9) & 7u;
            unsigned imm = op & 0xFFu;
            if (imm >= 0x80u)
                EFAIL("moveq immediate >= 0x80 needs sign-extend (deferred to [J5b])");
            out[n++] = mov_immed_u16(reg_d[dn], (uint16_t)imm, 0);
            continue;
        }

        if ((op & 0xF1F8u) == 0x5080u) {            /* addq.l #imm,Dn (Dn-direct, .L) */
            unsigned q   = (op >> 9) & 7u;
            unsigned imm = (q == 0) ? 8u : q;       /* q==0 => 8 */
            unsigned dn  = op & 7u;
            out[n++] = add_immed(reg_d[dn], reg_d[dn], (uint16_t)imm);  /* add w_dn,w_dn,#imm */
            continue;
        }

        if ((op & 0xF1F8u) == 0xD080u) {            /* add.l Dm,Dn (register-direct) */
            unsigned dn = (op >> 9) & 7u;
            unsigned dm = op & 7u;
            out[n++] = add_reg(reg_d[dn], reg_d[dn], reg_d[dm], LSL, 0);
            continue;
        }

        if ((op & 0xF1F8u) == 0x2010u) {            /* move.l (An),Dn — BE load */
            unsigned dn = (op >> 9) & 7u;
            unsigned an = op & 7u;
            int fidx = -1;
            n = emit_ea_indirect(out, n, reg_a[an], origin, size, &fidx);
            if (fidx >= 0) fault_idx[n_fault++] = fidx;
            /* host load (native little-endian), then REV to get the 68k big-endian value. */
            out[n++] = ldr_offset(SCR_ADDR, SCR_VAL, 0);            /* w_val = *(u32*)host */
            if (!neg_endianness)
                out[n++] = rev(SCR_VAL, SCR_VAL);                  /* big-endian -> host */
            out[n++] = mov_reg(reg_d[dn], SCR_VAL);                /* Dn = value */
            continue;
        }

        if ((op & 0xF1F8u) == 0x2080u) {            /* move.l Dn,(An) — BE store */
            unsigned an = (op >> 9) & 7u;
            unsigned dn = op & 7u;
            if (neg_skip_store) continue;           /* NEGATIVE CONTROL: omit the store */
            int fidx = -1;
            n = emit_ea_indirect(out, n, reg_a[an], origin, size, &fidx);
            if (fidx >= 0) fault_idx[n_fault++] = fidx;
            out[n++] = mov_reg(SCR_VAL, reg_d[dn]);                /* w_val = Dn (host order) */
            if (!neg_endianness)
                out[n++] = rev(SCR_VAL, SCR_VAL);                  /* host -> big-endian */
            out[n++] = str_offset(SCR_ADDR, SCR_VAL, 0);          /* *(u32*)host = w_val */
            continue;
        }

        if (op == 0x4E75u) { saw_rts = 1; break; }  /* rts — end of block */

        EFAIL("unsupported opcode in block ([J5a] subset: moveq/addq/add/move.l (An)<->Dn/rts)");
    }

    if (!saw_rts)
        EFAIL("block did not terminate in rts");

    /* Shared FAULT-EXIT landing pad: any out-of-range access branches here, having
     * already set state->fault. We fall through into the normal epilogue so the
     * (partial) register file + the fault flag are flushed and the block returns
     * cleanly. Patch every fault placeholder `b` to land on this word. */
    {
        int fault_pad = (int)n;
        for (int i = 0; i < n_fault; i++) {
            int rel = fault_pad - fault_idx[i];     /* forward distance in words */
            out[fault_idx[i]] = b(rel);             /* unconditional b, word-relative */
        }
    }

    /* Epilogue: flush D0..D7 and A0..A7 back to the state struct. */
    for (int i = 0; i < 8; i++) out[n++] = str_offset(0, reg_d[i], J5A_OFF_D(i));
    for (int i = 0; i < 8; i++) out[n++] = str_offset(0, reg_a[i], J5A_OFF_A(i));

    /* Restore callee-saved registers (reverse of the prologue) and ret. */
    out[n++] = ldp64_postindex(31, 27, 28, 16);
    out[n++] = ldp64_postindex(31, 25, 26, 16);
    out[n++] = ldp64_postindex(31, 23, 24, 16);
    out[n++] = ldp64_postindex(31, 21, 22, 16);
    out[n++] = ldp64_postindex(31, 19, 20, 16);
    out[n++] = ldp64_postindex(31, 29, 30, 16);
    out[n++] = ret();

    return n;
#undef EFAIL
}

/* The compiled block: AAPCS64 void(*)(state*, uint64_t base_adj). */
typedef void (*j5a_block_fn)(struct j5a_m68k_state *st, uint64_t base_adj);

int j5a_run_block(const j5a_sandbox *sb, uint32_t entry_pc, const uint8_t *code,
                  uint32_t code_len, struct j5a_m68k_state *st,
                  int neg_endianness, int neg_skip_store,
                  char *errbuf, unsigned errlen)
{
#define RFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)

    if (code_len == 0) RFAIL("empty entry block");

    uint32_t staging[512];
    unsigned nwords = emit_block(code, code_len, sb->origin, sb->size,
                                 neg_endianness, neg_skip_store,
                                 staging, errbuf, errlen);
    if (nwords == 0) return 1;             /* errbuf set by emit_block */

    if (jit_region_alloc(&g_region, nwords * sizeof(uint32_t)) != 0)
        RFAIL("jit_region_alloc(MAP_JIT) failed");
    g_region_live = 1;

    jit_write_begin(&g_region);
    memcpy(g_region.base, staging, nwords * sizeof(uint32_t));
    jit_write_end(&g_region);
    jit_finalize(&g_region, g_region.base, nwords * sizeof(uint32_t));

    st->pc = entry_pc;
    st->fault = 0;

    /* The adjusted sandbox base: host = base_adj + An. base_adj = host_mem - origin. */
    uint64_t base_adj = (uint64_t)(uintptr_t)sb->host_mem - (uint64_t)sb->origin;

    j5a_block_fn block = (j5a_block_fn)(void *)g_region.base;
    block(st, base_adj);                   /* <-- executes freshly-emitted AArch64 under W^X */

    return 0;
#undef RFAIL
}

void j5a_run_free(void)
{
    if (g_region_live) {
        jit_region_free(&g_region);
        g_region_live = 0;
    }
}
