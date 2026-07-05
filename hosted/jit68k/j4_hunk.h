/* j4_hunk.h — [J4] hunk loader + relocator + sandbox boundary (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Authored from docs/features/68k-jit/spec.md ([J4] + the
 * "AROS-side dispatch" section), the AmigaOS hunk format (a published standard,
 * [PUB]), and the REAL AROS loader/relocator in the upstream tree (cited inline,
 * [AROS]). This header contains NO Emu68 source — no Emu68 type, macro, or
 * declaration is copied here. It is the contact surface between:
 *   - j4_loader.c  (OURS) the minimal hunk loader + HUNK_RELOC32 relocator. Parses
 *                  a big-endian AmigaOS hunk file, allocates each hunk inside a
 *                  32-bit sandbox, and applies relocation exactly as the real AROS
 *                  loader does.
 *   - j4_build.c   (OURS) reuses the [J2] translate->emit path: hand-decodes the
 *                  register-only entry code hunk (moveq #N,d0 ; rts) and drives the
 *                  ADOPTED Emu68 emitter (emu68/A64.h) to fill a jit_region.
 *   - j4_test.c    (OURS) hand-assembles a REAL hunk binary, loads+relocates it,
 *                  runs the entry block, and value-asserts the relocated pointer +
 *                  the executed d0.
 *
 * =========================== GROUNDING (the real AROS contracts) ===============
 * Paths below are in the upstream AROS tree ../aros-upstream.
 *
 * (A) Hunk types — compiler/include/dos/doshunks.h:
 *       HUNK_HEADER  1011  (line 28)   HUNK_CODE   1001 (line 15)
 *       HUNK_DATA    1002  (line 16)   HUNK_BSS    1003 (line 17)
 *       HUNK_RELOC32 1004  (line 18)   HUNK_END    1010 (line 27)
 *     All hunk file longwords are 32-bit BIG-ENDIAN (the AmigaOS executable format).
 *
 * (B) Header layout — rom/dos/internalloadseg_aos.c:
 *       - optional library-name strings: read a BE32 length-in-longs; 0 ends the
 *         list (lines 126-137).
 *       - numhunks (BE32, line 138-141), first (BE32, 151-154), last (BE32, 158-161).
 *       - then (numhunks) per-hunk size words: lcount &= 0xFFFFFF; bytes = lcount*4
 *         (lines 177-180); the FAST/CHIP flag bits (top 2) are masked off; the
 *         FAST|CHIP combination reads an extra MEMF longword (lines 178-193). Our
 *         spike emits plain sizes (no flag bits set).
 *
 * (C) HUNK_RELOC32 — rom/dos/internalloadseg_aos.c:292-332. The exact algorithm we
 *     reproduce, byte-for-byte and big-endian:
 *       while (1) {
 *         count = BE32();  if (count==0) break;     // number of offsets
 *         targethunk = BE32();                       // hunk whose base is added
 *         for (count times) {
 *           offset = BE32();
 *           addr = GETHUNKPTR(lasthunk) + offset;    // patch site, in lasthunk
 *           val  = AROS_BE2LONG(*addr) + (IPTR)GETHUNKPTR(targethunk);
 *           *addr = AROS_LONG2BE(val);               // read BE32, add base, write BE32
 *         }
 *       }
 *     where `lasthunk` is the hunk whose CODE/DATA was most recently loaded (line
 *     288: `lasthunk = curhunk; ++curhunk`), and GETHUNKPTR(x) is the runtime base
 *     of hunk x (line 25). In our sandbox, GETHUNKPTR(x) == the 32-bit sandbox
 *     runtime address of hunk x's payload (sandbox-relative; see below).
 *
 * NOTE on GETHUNKPTR vs our sandbox. The real loader stores, per hunk, a [next-BPTR,
 * size, payload...] preamble and GETHUNKPTR skips the BPTR link (line 25). The
 * relocation VALUE that lands in memory is the *payload* runtime address. We model
 * exactly that: each hunk's `base` below is the 32-bit sandbox address of its
 * PAYLOAD (the first code/data byte), which is what a relocated 32-bit pointer must
 * equal. We do not need the seglist BPTR-chain shape for the relocation math — only
 * the payload runtime base per hunk, which is what GETHUNKPTR yields.
 * ===============================================================================
 */
#ifndef J4_HUNK_H
#define J4_HUNK_H

#include <stdint.h>
#include <stddef.h>

/* ----- (A) Hunk type constants — from doshunks.h [AROS] ------------------------ */
#define J4_HUNK_HEADER   1011u
#define J4_HUNK_CODE     1001u
#define J4_HUNK_DATA     1002u
#define J4_HUNK_BSS      1003u
#define J4_HUNK_RELOC32  1004u
#define J4_HUNK_SYMBOL   1008u   /* debug symbol table — SKIPPED (internalloadseg_aos.c:231) */
#define J4_HUNK_DEBUG    1009u   /* debug info        — SKIPPED (internalloadseg_aos.c:478) */
#define J4_HUNK_END      1010u

/* ----- The 32-bit sandbox -----------------------------------------------------
 * The 68k program's address space is a single host malloc'd region. A 68k (sandbox)
 * address `a` maps to the host byte `host_mem + a`. 68k addresses are 32-bit and
 * big-endian; everything inside the sandbox stays big-endian (the relocator writes
 * BE32 pointers, exactly like the real AROS loader).
 *
 * `next_alloc` is a simple bump allocator: each hunk gets the next aligned slice.
 * The sandbox base in 68k-space is `sandbox_origin` (a nonzero origin proves the
 * relocation adds a real per-hunk runtime base, not host pointer 0). */
typedef struct j4_sandbox {
    uint8_t  *host_mem;        /* host pointer to 68k address `sandbox_origin`     */
    uint32_t  sandbox_origin;  /* 68k-space address that host_mem[0] represents    */
    uint32_t  size;            /* sandbox size in bytes                            */
    uint32_t  next_alloc;      /* bump cursor, as a 68k-space (sandbox) address    */
} j4_sandbox;

/* Map a 68k sandbox address to its host pointer (and back). Both assert in-range
 * via the loader; here they are the raw arithmetic the spec's step-5 pointer
 * translation uses (sandbox_base + addr). */
static inline uint8_t *j4_sandbox_host(const j4_sandbox *sb, uint32_t addr)
{
    return sb->host_mem + (addr - sb->sandbox_origin);
}

/* ----- Loaded segment description ---------------------------------------------
 * After loading, each hunk has a 32-bit sandbox runtime base (the address of its
 * payload — what GETHUNKPTR yields and what relocations resolve to) and a size. */
#define J4_MAX_HUNKS 16

typedef struct j4_seglist {
    int       numhunks;                 /* number of hunks loaded               */
    uint32_t  hunk_base[J4_MAX_HUNKS];  /* 68k sandbox runtime address of hunk i payload */
    uint32_t  hunk_size[J4_MAX_HUNKS];  /* byte size of hunk i payload          */
    uint32_t  hunk_type[J4_MAX_HUNKS];  /* J4_HUNK_CODE / _DATA / _BSS          */
    uint32_t  entry;                    /* 68k entry PC = first CODE hunk base  */
} j4_seglist;

/* Initialise a sandbox over a host region of `size` bytes whose host_mem[0]
 * represents 68k address `origin`. Returns 0 on success. */
int  j4_sandbox_init(j4_sandbox *sb, uint8_t *host_mem, uint32_t origin, uint32_t size);

/* Load a big-endian AmigaOS hunk file (`buf`, `len` bytes) into `sb`, producing a
 * relocated `seglist`. Implements HUNK_HEADER parsing, HUNK_CODE/DATA/BSS payloads,
 * and HUNK_RELOC32 (big-endian, exactly as rom/dos/internalloadseg_aos.c). If
 * `skip_reloc` is nonzero the relocation pass is SKIPPED (the negative control that
 * proves the relocation assert bites). Returns 0 on success; on failure returns
 * nonzero and writes a reason into errbuf. */
int  j4_load_hunks(j4_sandbox *sb, const uint8_t *buf, size_t len,
                   int skip_reloc, j4_seglist *seglist,
                   char *errbuf, unsigned errlen);

/* ----- The 68k machine state at the run boundary (OURS layout) -----------------
 * Same shape as the [J2]/[J3] state; the JITed entry block reads/writes it via
 * 32-bit ldr/str against a base pointer, so field offsets are load-bearing. */
struct j4_m68k_state {
    uint32_t d[8];     /* data registers    D0..D7 : byte off  0.. 28 */
    uint32_t a[8];     /* address registers A0..A7 : byte off 32.. 60 */
    uint32_t ccr;      /* condition codes          : byte off 64      */
    uint32_t pc;       /* program counter          : byte off 68      */
};

#define J4_OFF_D(n)  ((uint16_t)((n) * 4u))          /* d[n] */
#define J4_OFF_A(n)  ((uint16_t)(32u + (n) * 4u))    /* a[n] */
#define J4_OFF_CCR   ((uint16_t)64u)
#define J4_OFF_PC    ((uint16_t)68u)

/* ----- The run path (j4_build.c) ----------------------------------------------
 * Translate the register-only entry code hunk into AArch64 via the ADOPTED Emu68
 * emitter (the [J2] path) and run it, returning the final 68k d0 (the program's
 * exit code) in *exit_d0. The entry block is read from the relocated sandbox at
 * `seglist->entry`. Returns 0 on success, nonzero on failure (errbuf set).
 *
 * SCOPE (carried from [J2]/[J3], stated in the spec's [J4]/[J5] split): this runs
 * only REGISTER-ONLY entry code (moveq / add / rts) through the [J2] hand-decode +
 * Emu68 emit path. The full Emu68 decoder + register-allocator lift (memory ops,
 * branches, real jsr-through-vector) is [J5]. */
int  j4_run_entry(j4_sandbox *sb, const j4_seglist *seglist, uint32_t *exit_d0,
                  char *errbuf, unsigned errlen);

/* Free any jit_region the run path allocated. */
void j4_run_free(void);

#endif /* J4_HUNK_H */
