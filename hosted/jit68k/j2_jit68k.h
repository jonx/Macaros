/* j2_jit68k.h — [J2] spike boundary types (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Authored from docs/features/68k-jit/spec.md ([J2]) and the
 * AArch64/68k ISA only. This header contains NO Emu68 source — no Emu68 type,
 * macro, or declaration is copied here. It is the contact surface between:
 *   - the JIT builder (j2_build.c, OURS) which hand-decodes a fixed 68k block and
 *     drives the *adopted Emu68 emitter* (emu68/A64.h) to fill a jit_region, and
 *   - the independent reference interpreter (j2_interp.c, OURS) which executes the
 *     same 68k block from scratch as the verification oracle, and
 *   - the test driver (j2_test.c, OURS) which diffs the two register files.
 *
 * The 68k machine state we keep in host memory. 68k registers are 32-bit. We keep
 * the whole architectural file flat so both the JITed block (via str/ldr against a
 * base pointer) and the interpreter operate on the identical layout, and the test
 * can memcmp/field-diff them. This mirrors Emu68's struct M68KState shape (D0-D7,
 * A0-A7, PC, SR/CCR) WITHOUT copying its declaration — the layout below is OURS.
 *
 * Field byte offsets (4-byte fields) are what the JIT builder feeds to the Emu68
 * str_offset/ldr_offset encoders, so keep them packed and in this order:
 *   d[0..7] : off  0,  4,  8, 12, 16, 20, 24, 28
 *   a[0..7] : off 32, 36, 40, 44, 48, 52, 56, 60
 *   ccr     : off 64
 *   pc      : off 68
 */
#ifndef J2_JIT68K_H
#define J2_JIT68K_H

#include <stdint.h>

struct m68k_state {
    uint32_t d[8];     /* data registers D0..D7   */
    uint32_t a[8];     /* address registers A0..A7 */
    uint32_t ccr;      /* condition codes (XNZVC packed in low bits, 68k order)   */
    uint32_t pc;       /* program counter */
};

/* Byte offsets of the fields above, for the JIT builder's str/ldr against x0. */
#define M68K_OFF_D(n)  ((uint16_t)((n) * 4u))          /* d[n] */
#define M68K_OFF_A(n)  ((uint16_t)(32u + (n) * 4u))    /* a[n] */
#define M68K_OFF_CCR   ((uint16_t)64u)
#define M68K_OFF_PC    ((uint16_t)68u)

/* 68k CCR bit positions (M68000 PRM): bit4=X bit3=N bit2=Z bit1=V bit0=C. */
#define CCR_C 0x01u
#define CCR_V 0x02u
#define CCR_Z 0x04u
#define CCR_N 0x08u
#define CCR_X 0x10u

/* The compiled 68k basic block: an AArch64 function taking the 68k state by
 * pointer (AAPCS64 x0), executing the translated block, and returning (the 68k
 * RTS lands here, the spec's central-dispatcher RET funnel). It both reads the
 * initial register file from *st and writes the final one back. */
typedef void (*jit68k_block_fn)(struct m68k_state *st);

/* j2_build.c (OURS): hand-decode the fixed 68k block
 *     moveq #10,d0 ; moveq #7,d1 ; add.l d1,d0 ; rts
 * into AArch64 via the adopted Emu68 emitter, write it into the MAP_JIT region,
 * finalize it, and return a callable function pointer (or NULL on failure).
 * `errbuf`/`errlen` receive a human-readable reason on failure. */
jit68k_block_fn jit68k_build_block(char *errbuf, unsigned errlen);

/* Free whatever jit68k_build_block allocated (the jit_region). */
void jit68k_free_block(void);

/* j2_interp.c (OURS, INDEPENDENT reference): execute the same fixed 68k block
 * from scratch over `st`. Must NOT use Emu68 in any way. */
void interp68k_run_block(struct m68k_state *st);

#endif /* J2_JIT68K_H */
