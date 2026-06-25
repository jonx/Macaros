/* j3_jit68k.h — [J3] LVO-call thunk boundary types (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Authored from docs/features/68k-jit/spec.md ([J3] + the
 * "integration boundary / C ABI" section) and the real AROS ABI headers in the
 * upstream tree (cited inline). This header contains NO Emu68 source — no Emu68
 * type, macro, or declaration is copied here. It is the contact surface between:
 *   - j3_vector.c  (OURS) the dispatch math: a 68k jsr-through-vector PC ->
 *                  (library base, LVO index n) via the REAL negative-offset rule.
 *   - j3_marshal.c (OURS) the marshaller: emits a reverse-H3 thunk via the ADOPTED
 *                  Emu68 emitter (emu68/A64.h, MPL quarantine) into a jit_region,
 *                  which reads the per-LVO source 68k registers from a M68KState and
 *                  blr's a native AAPCS64 stub, then stores the return into d0.
 *   - j3_test.c    (OURS) the driver: declares 2-3 native stubs with REAL register
 *                  macros, builds per-LVO descriptors, and value-asserts args+return.
 *
 * =========================== GROUNDING (the real AROS contracts) ===============
 * Paths below are in the upstream AROS tree /Users/user/Source/aros-upstream (this
 * project's source-of-truth for AROS headers; the aros-aarch64 workspace carries
 * the spikes + docs, the full tree lives upstream). All [AROS] facts are cited.
 *
 * (A) The dispatch math — arch/m68k-all/include/aros/cpu.h:
 *       struct JumpVec { unsigned short jmp; void *vec; };      (lines 50-54)
 *       #define __AROS_ASMJMP        0x4EF9                     (line 61)
 *       #define LIB_VECTSIZE         (sizeof (struct JumpVec))  (line 81)
 *       #define __AROS_GETJUMPVEC(lib,n)  (&(((struct JumpVec *)(lib))[-(n)])) (line 82)
 *     So vector n of a library sits at byte address  lib - n*sizeof(JumpVec), and a
 *     68k caller jumps there (a 6-byte `0x4EF9 <abs32>` FULLJMP, __AROS_USE_FULLJMP
 *     line 64). On the 68k TARGET, struct JumpVec is PACKED 6 bytes (2-byte jmp +
 *     4-byte 32-bit ptr; AROS_SIZEOFPTR==4, cpu.h line 19) => LIB_VECTSIZE == 6.
 *     We MUST use the 68k-ABI stride 6, NOT the host sizeof (which is 16 here, since
 *     the host void* is 8 bytes) — see J3_M68K_LIB_VECTSIZE below. This host/target
 *     divergence is exactly the kind of thing the spike must get right.
 *
 * (B) The per-function register map — the source 68k register for each argument:
 *       compiler/arossupport/include/libcall.h:1586
 *         #define AROS_LHA(type,name,reg)  type,name,reg
 *       compiler/arossupport/include/asmcall.h:822
 *         #define AROS_UFHA(type,name,reg) type,name,reg
 *     The third element `reg` is a symbolic 68k register (D0..D7, A0..A7). On m68k
 *     it resolves to the physical register:
 *       arch/m68k-all/include/aros/cpu.h:125-139   #define A0 a0 ... #define D7 d7
 *     and the call/declare machinery turns it into the asm register string
 *       arch/m68k-all/include/gencall.c:309  __AROS_LSA(type,name,reg) = "%"#reg
 *       arch/m68k-all/include/gencall.c:293  __AROS_UFSA(...)          = "%"#reg
 *     The CALLER side (gencall.c:60-201, asm_regs_init) literally does
 *       `register ULONG <reg>_tmp asm("%"#reg) = _arg<i>;  jsr o*LIB_VECTSIZE(%a6)`
 *     i.e. argument i is placed into 68k register `reg` before the library jump.
 *     => For LVO n, the ordered list of `reg`s in its AROS_LHA/AROS_UFHA declaration
 *     IS the authoritative table of which 68k register each argument arrives in.
 *     That ordered list of source registers is our per-LVO descriptor.
 *
 * (C) The return — gencall.c:153-157 (asm_regs_init tail):
 *       (sizeof(t) < sizeof(QUAD)) ? (t)(_ret0) : ...   with _ret0 asm("%d0")
 *     A 32-bit result is read back from 68k d0. => the bridge stores the native
 *     return into M68KState.d[0]. (line 66: register volatile ULONG _ret0 asm("%d0"))
 * ===============================================================================
 */
#ifndef J3_JIT68K_H
#define J3_JIT68K_H

#include <stdint.h>

/* ----- The 68k machine state at the dispatcher funnel (OURS layout) -----------
 * Mirrors the spec's `struct M68KState { uint32_t d[8],a[8],ccr,pc; }` and the
 * [J2] struct m68k_state, WITHOUT copying any Emu68 declaration. The marshaller
 * (emitted AArch64) reads/writes these via 32-bit ldr/str against a base pointer,
 * so the field offsets are load-bearing — keep packed in this order. */
struct M68KState {
    uint32_t d[8];     /* data registers    D0..D7 : byte off  0.. 28 */
    uint32_t a[8];     /* address registers A0..A7 : byte off 32.. 60 */
    uint32_t ccr;      /* condition codes          : byte off 64      */
    uint32_t pc;       /* program counter          : byte off 68      */
};

/* Byte offsets the emitted thunk feeds to the Emu68 ldr/str_offset encoders. */
#define M68K_OFF_D(n)  ((uint16_t)((n) * 4u))          /* d[n] */
#define M68K_OFF_A(n)  ((uint16_t)(32u + (n) * 4u))    /* a[n] */
#define M68K_OFF_CCR   ((uint16_t)64u)
#define M68K_OFF_PC    ((uint16_t)68u)

/* ----- (A) The dispatch math — the REAL 68k vector ABI -----------------------
 * struct JumpVec on the 68k TARGET is packed 6 bytes (UWORD jmp + 32-bit vec),
 * so LIB_VECTSIZE == 6 there (cpu.h:19,81). We hardcode the TARGET value here, NOT
 * host sizeof(struct JumpVec) (which would be 16: 2-byte short + 8-byte host ptr +
 * padding). Using 6 is the whole point of grounding against the m68k ABI. */
#define J3_M68K_LIB_VECTSIZE  6u

/* Symbolic source 68k registers for the per-LVO descriptor. These name a class
 * (data vs address) + index, matching the AROS_LHA/AROS_UFHA `reg` operand
 * (D0..D7 -> data, A0..A7 -> address). The enum value encodes class in the high
 * nibble so the marshaller can pick d[] vs a[] without a string compare. */
typedef enum {
    J3_D0 = 0x00, J3_D1, J3_D2, J3_D3, J3_D4, J3_D5, J3_D6, J3_D7,
    J3_A0 = 0x10, J3_A1, J3_A2, J3_A3, J3_A4, J3_A5, J3_A6, J3_A7
} j3_m68k_reg;

#define J3_REG_IS_ADDR(r)  (((r) & 0x10) != 0)   /* A-register? else D-register   */
#define J3_REG_INDEX(r)    ((r) & 0x0f)           /* 0..7 within its class         */

/* A per-LVO marshalling descriptor: the ORDERED list of source 68k registers,
 * one per native-call argument, taken DIRECTLY from the function's AROS_LHA /
 * AROS_UFHA declaration (grounding note B). nargs <= 8 (AAPCS64 x0..x7; >8 args
 * would spill to the stack — out of [J3] scope, all our descriptors are <= 8).
 * `stub` is the native AArch64 function the bridge calls (AAPCS64). `returns`
 * says whether the native return value is stored back into 68k d0 (note C). */
typedef struct {
    const char  *name;        /* human label for the trace            */
    uint32_t     libbase;     /* the library's 68k-space base address  */
    int          lvo;         /* positive LVO index n                  */
    int          nargs;       /* number of register args (0..8)        */
    j3_m68k_reg  src[8];      /* src[i] = 68k register feeding native arg i */
    int          returns;     /* 1: store native return into d0; 0: void */
    void       (*stub)(void); /* native AAPCS64 stub (cast per arity)   */
} j3_lvo_desc;

/* ----- (A) vector recognition (j3_vector.c) ----------------------------------
 * Given a library base and an LVO index n, compute the 68k-space address the
 * caller jumps to: libbase - n*J3_M68K_LIB_VECTSIZE. This IS __AROS_GETJUMPVEC
 * arithmetic restated with the target stride. */
uint32_t j3_vector_addr(uint32_t libbase, int n);

/* The inverse: given the jump-target PC and a candidate library base, recover the
 * LVO index n (and validate the PC lands exactly on a vector boundary at or below
 * the base). Returns the index >= 0 on a clean hit, or -1 if `pc` is not a valid
 * negative-offset vector of `libbase` (above the base, or not 6-byte aligned to
 * it). This is the dispatcher's "this jump lands in library L's vector n"
 * recognition, against the real cpu.h contract. */
int j3_vector_recognise(uint32_t libbase, uint32_t pc);

/* ----- (B)+(C) the marshaller (j3_marshal.c) ---------------------------------
 * Build (emit, via the ADOPTED Emu68 emitter into a jit_region) a native AArch64
 * thunk that, when called with a pointer to a M68KState in x0:
 *   - for each i in [0,nargs): reads the 32-bit 68k register desc->src[i] from the
 *     M68KState and places it (zero-extended) into AAPCS64 arg register x_i;
 *   - blr's desc->stub;
 *   - if desc->returns, stores the native 32-bit return (w0) into M68KState.d[0].
 * Returns a callable thunk, or NULL (errbuf set). This is the REVERSE of the H3
 * host-call shim: 68k regs -> AAPCS64, data-driven by the per-LVO register map. */
typedef void (*j3_thunk_fn)(struct M68KState *st);
j3_thunk_fn j3_build_marshal_thunk(const j3_lvo_desc *desc, char *errbuf, unsigned errlen);

/* Free the most recently built thunk's jit_region. (Spike keeps one live region
 * per built thunk in a small pool; see j3_marshal.c.) */
void j3_free_all_thunks(void);

#endif /* J3_JIT68K_H */
