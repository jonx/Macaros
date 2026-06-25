/* refcpu.h — a small INDEPENDENT 68k reference interpreter for the apps68k runner
 * (OURS, AROS-licensed).
 *
 * Clean-room / OURS, from the Motorola 68000 ISA only. This is NOT the JIT — it is
 * the oracle the runner uses to compute the EXPECTED result of programs that need
 * decoder coverage the JIT does not have yet ([J5c]): register-to-register move.l,
 * cmp.l+bne, post-increment memory loads (add.l (a0)+,d0), lea, and — crucially —
 * jsr-through-vector library calls, which it routes to a caller-supplied dispatch
 * callback (the stub library). It executes over the SAME [J4] sandbox the JIT uses,
 * so memory effects (loads, the relocated lea target) are real.
 *
 * It is deliberately a SUPERSET of the JIT's current opcode coverage, so the runner
 * can say "the JIT runs program X today; for program Y (needs [J5c]) here is the
 * expected result + observable effects this reference produces". It NEVER stands in
 * for a JIT pass — the runner reports JIT vs. reference status explicitly.
 *
 * Opcodes decoded (enough for mul/fact/arraysum/libcall):
 *   moveq #imm8,Dn                 0111 ddd0 iiiiiiii
 *   move.l #imm32,Dn               0010 ddd0 00 111 100  + imm32      (immediate->Dn)
 *   move.l #imm32,An (movea)       0010 aaa0 01 111 100  + imm32
 *   move.l Dm,Dn                   0010 ddd0 00 000 mmm                (reg->reg)
 *   move.l Dn,An (movea)           0010 aaa0 01 000 nnn
 *   move.l An,Am (movea)           0010 aaa0 01 001 nnn
 *   add.l  Dm,Dn                   1101 ddd0 10 000 mmm
 *   add.l  (An)+,Dn                1101 ddd0 10 011 aaa                (post-increment)
 *   addq.l #imm,Dn                 0101 qqq0 10 000 ddd
 *   subq.l #imm,Dn                 0101 qqq1 10 000 ddd
 *   cmp.l  Dm,Dn                   1011 ddd0 10 000 mmm
 *   lea    (d16,An)/abs,An         0100 aaa1 11 ...                    (we handle abs.l + (PC) forms vasm emits as 41F9 abs)
 *   bne.s  disp8                   0110 0110 dddddddd
 *   jsr    d16(A6)                 0100 1110 10 101 110 + d16          (library call via A6)
 *   rts                            0100 1110 0111 0101
 */
#ifndef APPS68K_REFCPU_H
#define APPS68K_REFCPU_H

#include <stdint.h>
#include "j4_hunk.h"

struct M68KState;  /* j3_jit68k.h — the state the dispatch callback marshals from */

/* The library-call dispatch hook: called when the reference decodes
 * `jsr d16(a6)`. `lvo` is the recovered positive LVO index (from the negative
 * offset via the cpu.h rule). The callback marshals the 68k registers (in `regs`)
 * into the native stub and may write a return into regs->d[0]. Returns 0 on a
 * dispatched LVO, nonzero to abort the run. `user` is passed through. */
typedef int (*refcpu_libcall_fn)(int lvo, struct M68KState *regs, void *user,
                                 char *errbuf, unsigned errlen);

/* Run the program at `entry_pc` (the relocated CODE hunk) over `sb`. `a6_libbase`
 * seeds A6 (the library base) so jsr d16(a6) resolves to libbase+d16; the negative
 * d16 maps to an LVO via (libbase - pc)/6. On a clean rts, the final register file
 * is in *regs and *exit_d0 = regs->d[0]. Returns 0 on success; nonzero on an
 * out-of-subset opcode or a dispatch error (errbuf set). Bounded by a step cap. */
int refcpu_run(j4_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
               struct M68KState *regs, uint32_t *exit_d0,
               refcpu_libcall_fn libcall, void *user,
               char *errbuf, unsigned errlen);

#endif /* APPS68K_REFCPU_H */
