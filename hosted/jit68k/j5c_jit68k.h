/* j5c_jit68k.h — [J5c] boundary types: RE-HOSTING Emu68's REAL decoder + register
 * allocator behind three hooks, to prove broad opcode coverage is reachable by adopting
 * Emu68's decode logic rather than hand-rolling it (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Authored from docs/features/68k-jit/spec.md ([J5]/[J5c]), the
 * Motorola 68000 ISA, and the AArch64 ISA only. This header contains NO Emu68 source —
 * no Emu68 type, macro, or declaration is copied here.
 *
 * ======================= WHAT [J5c] DOES DIFFERENTLY FROM [J5a]/[J5b] ============
 * [J5a]/[J5b] HAND-ROLLED the decode and only reused Emu68's *emitter* (A64.h), after
 * [J5a] found three couplings that block adopting the EA decoder + register allocator
 * incrementally. [J5c] takes the opposite, decisive bet: actually RE-HOST Emu68's REAL
 * per-opcode decoders (the verbatim, MPL-quarantined emu68/M68k_LINE{8,9,B,C,D}.c +
 * emu68/M68k_MOVE.c) and its REAL register-allocation *interface*, by PROVIDING hosted
 * replacements for exactly the three couplings [J5a] named, then DRIVING those real
 * decoders to translate a richer block than the hand-rolled path can reach.
 *
 * The three hosted hooks ([J5a]'s blockers, now bridged):
 *   HOOK 1 — sandbox memory / EA.  (j5c_shims.c + the policy below.) The EA decoder's
 *            (An) load/store emit `ldr/str w,[x_An]` treats the 68k An value as a raw
 *            host pointer (1:1 MMU, no sandbox base) AND assumes the CPU runs big-endian
 *            (Emu68 sets SCTLR.EE|E0E on bare metal — see the VERDICT note). On a
 *            little-endian hosted sandbox both are wrong and there is NO per-opcode hook
 *            to fix them. So [J5c] drives the REAL decoders for the REGISTER-DIRECT /
 *            ALU / immediate opcodes that touch NO 68k memory (where the real decoder
 *            lifts CLEANLY and runs correctly), and documents the memory-EA modes as the
 *            coupling that still forces editing M68k_EA.c. (Hook surface present so EA
 *            *compiles*; the richer block exercises mode-0 register-direct EA only.)
 *   HOOK 2 — instruction fetch.  (j5c_shims.c `cache_read_16`.) Emu68 reads every opcode
 *            and extension word via `cache_read_16(ICACHE, addr)` where `addr` is the
 *            HOST pointer to the instruction stream cast to uintptr_t (NOT a 68k address;
 *            see M68k_Translator.c / M68k_EA.c). Our shim is a direct BIG-ENDIAN 16-bit
 *            read from that host pointer — no Emu68 software ICache, no cache.c. Because
 *            the sandbox stores the 68k stream big-endian (the [J4] relocator wrote it
 *            that way), a BE read returns the architectural opcode on our LE host.
 *   HOOK 3 — RA SR/CTX.  (j5c_ra.c, OURS — our OWN register allocator implementing the
 *            RA_* interface declared in emu68/RegisterAllocator.h.) Emu68's
 *            RegisterAllocator64.c keeps the CCR in EL0 TPIDR_EL0 (`mrs/msr`) and the
 *            M68KState pointer in TPIDRRO_EL0 — a bare-metal convention we cannot use
 *            hosted. Our RA is memory-backed: D0..D7/A0..A7 stay in the fixed Emu68 map
 *            (w19..w26 / w13..w17,w27..w29), and RA_GetCC/RA_ModifyCC/RA_StoreCC keep the
 *            CCR in an ARM scratch register loaded from / stored to struct m68k_state
 *            (x0) — NOT a system register. (This is the [J5a] finding realised as a hook
 *            instead of a blocker: the RA *interface* is thin enough to re-host; only its
 *            EL0-sysreg SR/CTX *bodies* needed replacing.)
 *
 * A FOURTH coupling [J5c] discovered (recorded for the verdict): the Emu68 decoder
 * SOURCE is not byte-portable to the macOS toolchain — it uses GNU `__attribute__((
 * alias(...)))` function aliases, which clang/Mach-O REJECTS. The quarantined files stay
 * BYTE-VERBATIM; a build-time generator (emu68_darwinize.pl, OURS) rewrites the alias
 * chains into equivalent plain-C tail-call forwarders in build-dir copies. Disclosed in
 * emu68/NOTICE.
 * ===============================================================================
 *
 * THE BLOCK [J5c] translates THROUGH THE REAL DECODERS (register-direct, .L unless noted):
 *   moveq #imm8,Dn     EMIT_moveq  (M68k_MOVE.c)   — REAL sign-extend (generalises the
 *                                                    [J2]/[J4]/[J5b] imm<0x80 shortcut)
 *   add.l  Dm,Dn       EMIT_lineD  (EMIT_ADD)      — REAL adds + NZCVX
 *   sub.l  Dm,Dn       EMIT_line9  (EMIT_SUB)      — REAL subs + NZCVX (borrow inversion)
 *   and.l  Dm,Dn       EMIT_lineC  (EMIT_AND)      — REAL ands + NZ00
 *   or.l   Dm,Dn       EMIT_line8  (EMIT_OR)       — REAL orr  + NZ00
 *   eor.l  Dn,Dm       EMIT_lineB  (EMIT_EOR)      — REAL eor  + NZ00
 *   muls.w Dm,Dn       EMIT_lineC  (EMIT_MULS)     — REAL signed 16x16->32 + NZ00
 *   cmp.l  Dm,Dn       EMIT_lineB  (EMIT_CMP)      — REAL subs flags-only (NZnCV)
 * Eight opcodes across SIX different real Emu68 LINE decoders + the real EA + the real RA
 * + Emu68's REAL condition-code derivation (EMIT_GetNZCVX / EMIT_GetNZ00 / EMIT_GetNZnCV
 * in A64.h) — coverage the hand-rolled [J2]..[J5b] path never had.
 */
#ifndef J5C_JIT68K_H
#define J5C_JIT68K_H

#include <stdint.h>
#include <stddef.h>

/* ----- The hosted 68k machine state -------------------------------------------------
 * NOTE: the REAL Emu68 decoders + our RA reference m68k registers by NUMBER and keep the
 * architectural values in the fixed ARM register map at run time (D0..D7=w19..w26,
 * A0..A7=w13..w17,w27..w29). This struct is the MEMORY image our prologue loads those
 * registers FROM and the epilogue stores them BACK to, and the field the RA's CCR hook
 * loads/stores. Offsets are load-bearing (the prologue/epilogue and j5c_ra.c use them).
 *   d[0..7] : off  0,  4, ... 28
 *   a[0..7] : off 32, 36, ... 60
 *   ccr     : off 64   (low byte = 68k CCR; see the swap note in j5c_ra.c)
 *   pc      : off 68
 */
struct j5c_m68k_state {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t ccr;
    uint32_t pc;
};

#define J5C_OFF_D(n)   ((uint16_t)((n) * 4u))
#define J5C_OFF_A(n)   ((uint16_t)(32u + (n) * 4u))
#define J5C_OFF_CCR    ((uint16_t)64u)
#define J5C_OFF_PC     ((uint16_t)68u)

/* 68k CCR bit positions (M68000 PRM): bit4=X bit3=N bit2=Z bit1=V bit0=C. */
#define J5C_CCR_C 0x01u
#define J5C_CCR_V 0x02u
#define J5C_CCR_Z 0x04u
#define J5C_CCR_N 0x08u
#define J5C_CCR_X 0x10u

/* ----- The sandbox (carried from [J5a]/[J4]) ---------------------------------------
 * The 68k code stream lives in this host-backed region. cache_read_16 (HOOK 2) fetches
 * opcodes/extension words big-endian from it. (Data memory accesses via (An) are the EA
 * coupling [J5c] documents but does not exercise in the richer block — register-direct
 * only.) */
typedef struct j5c_sandbox {
    uint8_t  *host_mem;   /* host pointer to 68k address `origin`                 */
    uint32_t  origin;     /* 68k-space base address that host_mem[0] represents   */
    uint32_t  size;       /* sandbox size in bytes                                */
} j5c_sandbox;

/* ----- The run path (j5c_build.c) ---------------------------------------------------
 * Translate the entry block (read from the sandbox at `entry_pc`, `code_len` bytes)
 * THROUGH THE REAL EMU68 DECODERS (emu68/M68k_LINE*.c + M68k_MOVE.c + M68k_EA.c, driven
 * via the EmitINSN-style line dispatch), with the three hosted hooks, run it under W^X
 * over a seeded state, and write the final 68k register file (incl. the real CCR) back
 * into *st. Returns 0 on success, nonzero on a decode/emit/setup error (errbuf set).
 *
 * `neg_corrupt_decode`, when nonzero, is a NEGATIVE CONTROL: it perturbs the opcode
 * stream fed to the real decoder (flips an opcode) so the JIT result must diverge from
 * the independent interpreter — proving the asserts bite. */
int j5c_run_block(const j5c_sandbox *sb, uint32_t entry_pc, const uint8_t *code,
                  uint32_t code_len, struct j5c_m68k_state *st,
                  int neg_corrupt_decode, char *errbuf, unsigned errlen);

void j5c_run_free(void);

/* ----- The independent reference (j5c_interp.c, OURS — no Emu68) --------------------
 * Execute the same block from scratch (no Emu68) over `st`, computing the architectural
 * result of every opcode AND the full 68k CCR (N/Z/V/C/X) by the M68000 rules, so the
 * test can assert the JITed register file and CCR are byte-exact equal to the reference.
 */
void j5c_interp_run_block(uint32_t entry_pc, const uint8_t *code, uint32_t code_len,
                          struct j5c_m68k_state *st);

#endif /* J5C_JIT68K_H */
