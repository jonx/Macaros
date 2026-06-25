/* j5a_jit68k.h — [J5a] boundary types: memory load/store + the sandbox-pointer
 * boundary (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Authored from docs/features/68k-jit/spec.md ([J5]/[J5a]), the
 * Motorola 68000 ISA, and the AArch64 ISA only. This header contains NO Emu68
 * source — no Emu68 type, macro, or declaration is copied here. It is the contact
 * surface between:
 *   - j5a_build.c   (OURS) the HAND-ROLLED EA decode + sandbox memory-access path.
 *                   It drives the *adopted Emu68 emitter* (emu68/A64.h) — calling
 *                   its inline AArch64 encoders, exactly as [J2]/[J3]/[J4] did — to
 *                   translate a small block that touches memory via A0, mapping the
 *                   68k address register to a host pointer (sandbox_base + addr)
 *                   around every load/store, big-endian, bounds-checked.
 *   - j5a_interp.c  (OURS, INDEPENDENT reference) which executes the same block from
 *                   scratch (no Emu68) over a model of the SAME sandbox, so the test
 *                   can assert the JITed register file AND the sandbox memory bytes
 *                   are byte-exact equal to the interpreter's.
 *   - j5a_test.c    (OURS) the value-asserting driver + negative controls.
 *
 * =========================== WHY HAND-ROLLED (the [J5a] finding) ================
 * The spec's [J5a] task is: adopt Emu68's EA decode (src/M68k_EA.c) + register
 * allocator (src/aarch64/RegisterAllocator64.c + include/RegisterAllocator.h) to
 * drive the A64 emitter for memory opcodes IF it lifts cleanly; otherwise HAND-ROLL
 * and report precisely what Emu68 coupling blocked the incremental lift. It does
 * NOT lift cleanly for our hosted sandbox model. Three concrete, citable blockers
 * (Emu68 commit 305f686, v1.0.7):
 *
 *   (1) `An` is dereferenced as a HOST pointer, with no sandbox base.
 *       In M68k_EA.c EMIT_LoadFromEffectiveAddress(), mode 2 `(An)` size 4 emits:
 *           uint8_t reg_An = RA_MapM68kRegister(&ptr, src_reg + 8);
 *           *ptr++ = ldr_offset(reg_An, *arm_reg, 0);   // ldr w_dst,[x_An]
 *       i.e. it loads from the raw 68k address register AS IF it were a native host
 *       address (M68k_EA.c:635-639; the store mirror is in
 *       EMIT_StoreToEffectiveAddress). Emu68 can do this because bare-metal it maps
 *       the 68k address space 1:1 onto host RAM via its MMU (src/aarch64/mmu.c — NOT
 *       lifted). Our sandbox needs host = sandbox_base + An; there is NO hook inside
 *       the per-opcode emit to insert that base add, so adopting EA would require
 *       EDITING the MPL file at the load/store site — surgery INSIDE the opcode emit,
 *       not at the unit boundary the spec's adaptation note keeps clean.
 *
 *   (2) Every extension word is read through Emu68's SOFTWARE instruction cache.
 *       M68k_EA.c reads displacements/immediates via cache_read_16(ICACHE, ...) at
 *       ~26 sites (e.g. M68k_EA.c:760,765,772,...). ICACHE/cache_read_16 are Emu68's
 *       software m68k cache (include/cache.h, src/cache.c — its bare-metal D/I cache
 *       model). Adopting M68k_EA.c pulls in cache.h + cache.c + the ICACHE/DCACHE
 *       enum and its address model wholesale — far beyond a memory-op increment.
 *
 *   (3) The register allocator assumes SR/context live in EL0 system registers.
 *       RegisterAllocator64.c reads the condition codes with mrs(reg,3,3,13,0,2)
 *       (TPIDR_EL0) and the M68KState pointer with mrs(...,3,3,13,0,3) (TPIDRRO_EL0)
 *       — RegisterAllocator64.c:288-303 (RA_GetCC), :175-185 (RA_GetCTX). That is a
 *       bare-metal convention (Emu68 owns those thread-ID system registers); our
 *       hosted blocks pass the state pointer in x0 and keep CCR in the M68KState
 *       struct in memory. Also support.h / M68k.h (struct M68KState) come along.
 *
 * CONCLUSION (a primary [J5a] deliverable): the EA decode + register allocator do
 * NOT lift piecemeal — they are coupled to (a) Emu68's 1:1 MMU address model, (b)
 * its software instruction cache, and (c) its EL0-system-register SR/context model,
 * all three parts of the bare-metal runtime [J0] says we re-host. We therefore build
 * our OWN tiny EA/memory path AROUND the adopted *emitter* (A64.h), which DID lift
 * cleanly ([J2] finding) and is all we reuse. The emitter encoders (ldr_offset,
 * str_offset, add_reg, rev, add_immed, cmp_immed/b_cc) carry all the AArch64
 * encoding; the sandbox-base add, the big-endian byteswap, and the bounds-check are
 * the small OURS logic the adoption could not provide. The larger RA mountain ([J5b])
 * will be OUR register allocator built around the emitter, not Emu68's.
 * ===============================================================================
 */
#ifndef J5A_JIT68K_H
#define J5A_JIT68K_H

#include <stdint.h>
#include <stddef.h>

/* ----- The 68k machine state (same flat layout as [J2]/[J4]) -------------------
 * 68k registers are 32-bit. The JITed block reads/writes this via ldr/str against
 * a base pointer (x0), and the interpreter operates on the identical layout, so the
 * test can field-diff the two. Offsets are load-bearing (fed to str_offset/ldr_offset).
 *   d[0..7] : off  0,  4,  8, 12, 16, 20, 24, 28
 *   a[0..7] : off 32, 36, 40, 44, 48, 52, 56, 60
 *   ccr     : off 64
 *   pc      : off 68
 *   fault   : off 72   (set nonzero by the emitted block on an out-of-range access)
 */
struct j5a_m68k_state {
    uint32_t d[8];     /* data registers    D0..D7 */
    uint32_t a[8];     /* address registers A0..A7 */
    uint32_t ccr;      /* condition codes (XNZVC packed, 68k bit order)            */
    uint32_t pc;       /* program counter                                          */
    uint32_t fault;    /* 0 = clean; nonzero = a memory access went out of sandbox */
};

#define J5A_OFF_D(n)   ((uint16_t)((n) * 4u))          /* d[n] */
#define J5A_OFF_A(n)   ((uint16_t)(32u + (n) * 4u))    /* a[n] */
#define J5A_OFF_CCR    ((uint16_t)64u)
#define J5A_OFF_PC     ((uint16_t)68u)
#define J5A_OFF_FAULT  ((uint16_t)72u)

/* 68k CCR bit positions (M68000 PRM): bit4=X bit3=N bit2=Z bit1=V bit0=C. */
#define J5A_CCR_C 0x01u
#define J5A_CCR_V 0x02u
#define J5A_CCR_Z 0x04u
#define J5A_CCR_N 0x08u
#define J5A_CCR_X 0x10u

/* A nonzero fault code the emitted bounds-check writes when An is out of range. */
#define J5A_FAULT_OOB 0xBADADD12u

/* ----- The sandbox memory-access boundary (the [J5a] core) ---------------------
 * The 68k program's address space is a single host region. A 68k (sandbox) address
 * `a` maps to host byte `host_mem + (a - origin)`. The emitted block computes, for a
 * memory op through An:
 *     host_ptr = sandbox_base_host + (An - origin)
 * where sandbox_base_host is `host_mem` and origin is the 68k-space base. We pass
 * the *adjusted* base  (host_mem - origin)  as a single 64-bit value to the block so
 * the per-access add is exactly `host_mem_adj + An`. Loads/stores are big-endian
 * (68k); the block byteswaps with REV. An is bounds-checked: An and An+size must lie
 * within [origin, origin+size) or the block faults cleanly (sets state->fault and
 * returns without touching host memory out of range). */
typedef struct j5a_sandbox {
    uint8_t  *host_mem;   /* host pointer to 68k address `origin`                 */
    uint32_t  origin;     /* 68k-space base address that host_mem[0] represents   */
    uint32_t  size;       /* sandbox size in bytes                                */
} j5a_sandbox;

static inline uint8_t *j5a_sandbox_host(const j5a_sandbox *sb, uint32_t addr)
{
    return sb->host_mem + (addr - sb->origin);
}

/* ----- The run path (j5a_build.c) ----------------------------------------------
 * Translate the memory-touching entry block (read from the relocated sandbox at
 * `entry_pc`, `code_len` bytes) into AArch64 via the adopted Emu68 *emitter* with the
 * hand-rolled EA/memory path, run it under W^X over a seeded state + the sandbox, and
 * write the final 68k register file back into *st. Returns 0 on success, nonzero on a
 * decode/emit/setup error (errbuf set). The block itself reports an out-of-range
 * access by setting st->fault (it never dereferences out of the sandbox).
 *
 * `neg_endianness`, when nonzero, is a NEGATIVE CONTROL: the load/store is emitted
 * WITHOUT the big-endian REV byteswap, so a JIT-vs-interpreter diff must appear
 * (proving the asserts bite). `neg_skip_store`, when nonzero, SKIPS emitting the
 * store opcode (another negative control). */
int j5a_run_block(const j5a_sandbox *sb, uint32_t entry_pc, const uint8_t *code,
                  uint32_t code_len, struct j5a_m68k_state *st,
                  int neg_endianness, int neg_skip_store,
                  char *errbuf, unsigned errlen);

/* Free whatever the run path allocated (the jit_region). */
void j5a_run_free(void);

/* ----- The independent reference (j5a_interp.c, OURS — no Emu68) ----------------
 * Execute the same memory-touching block from scratch over `st` and the model of the
 * SAME sandbox `sb`. It performs big-endian loads/stores against the sandbox bytes,
 * so after running, BOTH the register file in *st and the sandbox memory bytes are
 * the reference the test asserts the JIT matches, byte-exact. */
void j5a_interp_run_block(const j5a_sandbox *sb, uint32_t entry_pc,
                          const uint8_t *code, uint32_t code_len,
                          struct j5a_m68k_state *st);

#endif /* J5A_JIT68K_H */
