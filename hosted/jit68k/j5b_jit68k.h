/* j5b_jit68k.h — [J5b] boundary types: control flow (a real loop with a conditional
 * backward branch) + real condition codes (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Authored from docs/features/68k-jit/spec.md ([J5]/[J5b]), the
 * Motorola 68000 ISA, and the AArch64 ISA only. This header contains NO Emu68
 * source — no Emu68 type, macro, or declaration is copied here. It is the contact
 * surface between:
 *   - j5b_build.c   (OURS) the HAND-ROLLED loop decode + real-CCR emit. It drives the
 *                   *adopted Emu68 emitter* (emu68/A64.h) — calling its inline AArch64
 *                   encoders, exactly as [J2]/[J3]/[J4]/[J5a] did — to translate a
 *                   self-contained 68k loop into a SINGLE jit_region with an INTERNAL
 *                   backward branch (an AArch64 b.ne to a label at the loop top),
 *                   computing REAL condition codes for the branch.
 *   - j5b_interp.c  (OURS, INDEPENDENT reference) which executes the same loop from
 *                   scratch (no Emu68), counting iterations, so the test can assert the
 *                   JITed register file AND the iteration count / termination match.
 *   - j5b_test.c    (OURS) the value-asserting driver + the negative control.
 *
 * ===================== REAL CONDITION CODES (the [J5b] core) ====================
 * [J2]/[J4]'s blocks statically folded CCR=0 (valid only because those operands set no
 * flags); [J5a] computed move/add flags in the *interpreter* but the emitted block did
 * not need a branch. [J5b] needs a GENUINE branch condition:
 *
 *   subq.l #1,d1   ->   subs w_d1, w_d1, #1   (the FLAG-SETTING subtract, A64.h)
 *
 * The conditional `bne.s L` is emitted as an AArch64 `b.ne` (A64_CC_NE, Z==0) that
 * consumes the NZCV the `subs` just produced — i.e. the AArch64 flags ARE the live 68k
 * branch condition (no recompute needed for the branch itself; at minimum Z is exact).
 *
 * The 68k CCR (state->ccr) is ALSO recomputed to the full N/Z/V/C/X — but with
 * NON-flag-setting ops (cset/orr/and/str), emitted BETWEEN the `subs` and the `b.ne`,
 * so the branch still sees the subs flags. 68k subtract flag rule (M68000 PRM, .L):
 *   N = result bit31 ; Z = (result == 0) ; V = signed overflow of dst - src ;
 *   C = borrow (i.e. src > dst, unsigned) ; X := C.
 * NOTE the C/X subtlety: AArch64 SUBS sets C = NOT-borrow (C=1 means no borrow), the
 * OPPOSITE of the 68k C bit. The emit derives 68k-C from the borrow condition
 * (A64_CC_CC = "carry clear" = AArch64 C==0 = a borrow occurred), so the stored CCR
 * matches the interpreter's subtract rule exactly.
 * ===============================================================================
 *
 * ============================ SINGLE-REGION BACKWARD BRANCH =====================
 * The WHOLE loop is emitted ONCE into a single jit_region. The loop top is a label —
 * a word index into the emitted stream. The `b.ne` at the bottom branches back to it
 * with a NEGATIVE word offset (loop_top_idx - bne_idx). A64.h's b_cc/b take signed
 * word offsets and mask the two's-complement low bits; ASSERT_OFFSET is a no-op at
 * -O2, so backward branches encode cleanly. No cross-region chaining ([J5c]).
 * ===============================================================================
 *
 * Opcode subset (the [J5b] loop):
 *   moveq #imm8,Dn   (imm8 < 0x80 — the [J2] zero/sign-extend shortcut, enforced)
 *   add.l  Dm,Dn     (register-direct .L)
 *   subq.l #imm,Dn   (imm in 1..8, q=0 => 8) — sets REAL flags (the branch source)
 *   bne.s  disp8     (PC-relative 8-bit signed; the backward branch closing the loop)
 *   rts
 * DEFERRED to [J5c]: cross-region chaining + instruction cache, full Bcc/DBcc coverage,
 * forward branches across blocks, real jsr-through-vector decode from a stream, library
 * calls from the running program, and OUR register allocator (still memory-backed here).
 */
#ifndef J5B_JIT68K_H
#define J5B_JIT68K_H

#include <stdint.h>
#include <stddef.h>

/* ----- The 68k machine state (same flat layout as [J2]/[J4]/[J5a]) --------------
 * 68k registers are 32-bit. The JITed block reads/writes this via ldr/str against a
 * base pointer (x0); the interpreter operates on the identical layout, so the test can
 * field-diff the two. Offsets are load-bearing (fed to str_offset/ldr_offset).
 *   d[0..7] : off  0,  4,  8, 12, 16, 20, 24, 28
 *   a[0..7] : off 32, 36, 40, 44, 48, 52, 56, 60
 *   ccr     : off 64
 *   pc      : off 68
 */
struct j5b_m68k_state {
    uint32_t d[8];     /* data registers    D0..D7 */
    uint32_t a[8];     /* address registers A0..A7 */
    uint32_t ccr;      /* condition codes (XNZVC packed, 68k bit order)            */
    uint32_t pc;       /* program counter                                          */
};

#define J5B_OFF_D(n)   ((uint16_t)((n) * 4u))          /* d[n] */
#define J5B_OFF_A(n)   ((uint16_t)(32u + (n) * 4u))    /* a[n] */
#define J5B_OFF_CCR    ((uint16_t)64u)
#define J5B_OFF_PC     ((uint16_t)68u)

/* 68k CCR bit positions (M68000 PRM): bit4=X bit3=N bit2=Z bit1=V bit0=C. */
#define J5B_CCR_C 0x01u
#define J5B_CCR_V 0x02u
#define J5B_CCR_Z 0x04u
#define J5B_CCR_N 0x08u
#define J5B_CCR_X 0x10u

/* ----- The run path (j5b_build.c) ----------------------------------------------
 * Translate the loop entry block (read from the relocated sandbox at `entry_pc`,
 * `code_len` bytes) into AArch64 via the adopted Emu68 *emitter* with the hand-rolled
 * loop decode + real-CCR emit, run it under W^X over a seeded state, and write the
 * final 68k register file (incl. the real CCR) back into *st. Returns 0 on success,
 * nonzero on a decode/emit/setup error (errbuf set).
 *
 * `neg_break_branch`, when nonzero, is a NEGATIVE CONTROL: the conditional backward
 * branch is emitted with the WRONG condition (always-taken) so the loop never
 * terminates on its own — the watchdog (in the test) must catch the hang, proving the
 * termination assert bites. The test runs this control in a forked child so the parent
 * survives.
 */
int j5b_run_block(uint32_t entry_pc, const uint8_t *code, uint32_t code_len,
                  struct j5b_m68k_state *st, int neg_break_branch,
                  char *errbuf, unsigned errlen);

/* Free whatever the run path allocated (the jit_region). */
void j5b_run_free(void);

/* ----- The independent reference (j5b_interp.c, OURS — no Emu68) -----------------
 * Execute the same loop from scratch over `st`, following the real backward branch and
 * counting loop iterations into *iters (NULL = don't count). It bounds its own work
 * (a generous instruction-step cap) so a mis-decoded reference cannot itself hang the
 * test; on hitting the cap it sets *terminated = 0. Returns with the final register
 * file (incl. the full N/Z/V/C/X CCR) in *st. */
void j5b_interp_run_block(uint32_t entry_pc, const uint8_t *code, uint32_t code_len,
                          struct j5b_m68k_state *st, uint32_t *iters, int *terminated);

#endif /* J5B_JIT68K_H */
