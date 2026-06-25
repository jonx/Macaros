/* stublib.c — the minimal STUB AmigaOS library environment (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Implements stublib.h: builds the negative-offset JumpVec table
 * in the [J4] sandbox per arch/m68k-all/include/aros/cpu.h, provides native stub
 * functions for AllocMem/FreeMem/PutChar that RECORD their calls + args, and
 * dispatches a recognised LVO call through the REAL [J3] marshaller
 * (j3_build_marshal_thunk) — the 68k-regs -> AAPCS64 reverse-H3 thunk emitted into a
 * MAP_JIT region. This makes a library-calling 68k program's behaviour observable.
 *
 * The native stubs need the per-call argument values AND a way to touch the harness
 * state (the heap cursor, the call log). The [J3] marshaller calls a plain AAPCS64
 * stub `(uint32_t,...)->uint32_t`, so we thread the active stub_lib through a single
 * file-scope pointer set just before each dispatch (single-threaded harness).
 */
#include "stublib.h"
#include "j3_jit68k.h"

#include <string.h>
#include <stdio.h>

/* ----- big-endian sandbox writers (the JumpVec bytes are 68k big-endian) ------- */
static void sb_w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void sb_w32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

/* The library currently being dispatched (single-threaded harness). The native
 * stubs read/write it. Set by stublib_dispatch before the marshal thunk runs. */
static stub_lib *g_active;

/* ----- native AAPCS64 stub functions (the bodies the [J3] thunk blr's) ----------
 * Each takes the 68k register args (already marshalled into x0.. by the thunk, per
 * the per-LVO src[] map) and records the call. These are ordinary host functions;
 * the marshaller turns 68k regs into their AAPCS64 args. */

/* AllocMem(d0=byteSize, d1=requirements) -> d0 = 68k sandbox pointer (or 0).
 * AROS_LHA map: (ULONG byteSize, D0), (ULONG requirements, D1). */
static uint32_t stub_AllocMem(uint32_t byteSize, uint32_t requirements)
{
    stub_lib *L = g_active;
    uint32_t aligned = (byteSize + 15u) & ~15u;         /* MEMCHUNK rounding */
    uint32_t ptr = 0;
    if (aligned && L->heap_next + aligned <= L->heap_end) {
        ptr = L->heap_next;
        L->heap_next += aligned;
        L->bytes_outstanding += byteSize;
    }
    /* record */
    if (L->ncalls < STUB_MAX_CALLS) {
        stub_call_rec *r = &L->calls[L->ncalls++];
        r->lvo = STUB_LVO_ALLOCMEM; r->arg_d0 = byteSize; r->arg_d1 = requirements;
        r->arg_a1 = 0; r->ret_d0 = ptr;
    }
    (void)requirements;   /* MEMF_CLEAR honoured implicitly: sandbox is zero-init */
    return ptr;
}

/* FreeMem(a1=memoryBlock, d0=byteSize) -> void.
 * AROS_LHA map: (APTR memoryBlock, A1), (ULONG byteSize, D0). */
static uint32_t stub_FreeMem(uint32_t memoryBlock, uint32_t byteSize)
{
    stub_lib *L = g_active;
    if (byteSize <= L->bytes_outstanding) L->bytes_outstanding -= byteSize;
    if (L->ncalls < STUB_MAX_CALLS) {
        stub_call_rec *r = &L->calls[L->ncalls++];
        r->lvo = STUB_LVO_FREEMEM; r->arg_d0 = byteSize; r->arg_d1 = 0;
        r->arg_a1 = memoryBlock; r->ret_d0 = 0;
    }
    return 0;   /* void LVO; the descriptor sets returns=0 so d0 is untouched */
}

/* PutChar(d0=char) -> void. Observable "print" sink. */
static uint32_t stub_PutChar(uint32_t ch)
{
    stub_lib *L = g_active;
    if (L->outlen < (int)sizeof(L->out) - 1) L->out[L->outlen++] = (char)(ch & 0xff);
    if (L->ncalls < STUB_MAX_CALLS) {
        stub_call_rec *r = &L->calls[L->ncalls++];
        r->lvo = STUB_LVO_PUTCHAR; r->arg_d0 = ch; r->arg_d1 = 0;
        r->arg_a1 = 0; r->ret_d0 = 0;
    }
    return 0;
}

/* ----- the LVO -> (native stub, register map) table ----------------------------
 * Each entry is a [J3] descriptor: the ORDERED source 68k registers (the
 * AROS_LHA/AROS_UFHA `reg` list) feeding the native stub, and whether the return
 * goes back into d0. Grounded in j3_jit68k.h note B (the reg list IS the map). */
static const j3_lvo_desc *lvo_desc(int lvo)
{
    /* Built lazily; the libbase field is filled in by stublib_dispatch (it varies
     * per library instance). nargs/src/returns are the fixed AROS_LH maps. */
    static j3_lvo_desc allocmem = {
        "AllocMem", 0, STUB_LVO_ALLOCMEM, 2, { J3_D0, J3_D1 }, 1, (void(*)(void))stub_AllocMem
    };
    static j3_lvo_desc freemem = {
        "FreeMem", 0, STUB_LVO_FREEMEM, 2, { J3_A1, J3_D0 }, 0, (void(*)(void))stub_FreeMem
    };
    static j3_lvo_desc putchar_ = {
        "PutChar", 0, STUB_LVO_PUTCHAR, 1, { J3_D0 }, 0, (void(*)(void))stub_PutChar
    };
    switch (lvo) {
        case STUB_LVO_ALLOCMEM: return &allocmem;
        case STUB_LVO_FREEMEM:  return &freemem;
        case STUB_LVO_PUTCHAR:  return &putchar_;
        default:                return NULL;
    }
}

/* ----- the JumpVec table builder ----------------------------------------------- */
int stublib_init(stub_lib *lib, j4_sandbox *sb, uint32_t libbase,
                 uint32_t heap_base, uint32_t heap_end)
{
    memset(lib, 0, sizeof(*lib));
    lib->libbase   = libbase;
    lib->heap_base = heap_base;
    lib->heap_end  = heap_end;
    lib->heap_next = heap_base;

    /* Lay down a FULLJMP vector (0x4EF9 <sentinel32>) for each implemented LVO at
     * byte address libbase - n*6 (cpu.h __AROS_GETJUMPVEC, LIB_VECTSIZE==6). The
     * <sentinel> target is never executed by the harness; the dispatcher recognises
     * the jsr-through-vector PC and routes to the native stub. We still write the
     * real 0x4EF9 so the table is byte-faithful to what MakeLibrary would build. */
    const int lvos[] = { STUB_LVO_PUTCHAR, STUB_LVO_ALLOCMEM, STUB_LVO_FREEMEM };
    for (unsigned i = 0; i < sizeof(lvos)/sizeof(lvos[0]); i++) {
        int n = lvos[i];
        uint32_t vaddr = libbase - (uint32_t)n * J3_M68K_LIB_VECTSIZE;   /* lib - n*6 */
        /* bounds: the vector (6 bytes) must lie inside the sandbox */
        if (vaddr < sb->sandbox_origin ||
            (uint64_t)vaddr + 6u > (uint64_t)sb->sandbox_origin + sb->size)
            return 1;
        uint8_t *p = j4_sandbox_host(sb, vaddr);
        sb_w16(p, 0x4EF9u);                              /* __AROS_ASMJMP */
        sb_w32(p + 2, 0xC0ED0000u | (uint32_t)n);        /* sentinel (cpu.h _aros_empty_vector style) */
    }
    return 0;
}

uint32_t stublib_vector_addr(const stub_lib *lib, int lvo)
{
    return lib->libbase - (uint32_t)lvo * J3_M68K_LIB_VECTSIZE;
}

/* Per-LVO marshal-thunk cache. The [J3] marshaller emits one MAP_JIT region per build and
 * holds a small pool (J3_MAX_THUNKS). A library-calling program in a LOOP (e.g. the [J5j]
 * Mandelbrot, which calls PutChar ~1700 times) would exhaust that pool if a fresh thunk were
 * built per call. The thunk for a given LVO is FIXED (its source-register map never changes),
 * so build it ONCE and reuse it — exactly what a real library base does (the JumpVec is set
 * up once at MakeLibrary time). Keyed by the LVO index; the three stub LVOs need three thunks
 * total, well within the pool. (Single-threaded harness; no locking needed.) */
typedef struct { int lvo; j3_thunk_fn thunk; } thunk_cache_ent;
static thunk_cache_ent g_thunk_cache[8];
static int             g_thunk_cache_n = 0;

static j3_thunk_fn cached_thunk(const j3_lvo_desc *desc, char *errbuf, unsigned errlen)
{
    for (int i = 0; i < g_thunk_cache_n; i++)
        if (g_thunk_cache[i].lvo == desc->lvo) return g_thunk_cache[i].thunk;
    j3_thunk_fn t = j3_build_marshal_thunk(desc, errbuf, errlen);
    if (!t) return NULL;
    if (g_thunk_cache_n < (int)(sizeof g_thunk_cache / sizeof g_thunk_cache[0])) {
        g_thunk_cache[g_thunk_cache_n].lvo = desc->lvo;
        g_thunk_cache[g_thunk_cache_n].thunk = t;
        g_thunk_cache_n++;
    }
    return t;
}

/* Drop the thunk cache (the underlying regions are freed by j3_free_all_thunks). Call this
 * after each program run so a fresh run rebuilds against a fresh [J3] pool. */
void stublib_reset_thunk_cache(void) { g_thunk_cache_n = 0; }

int stublib_dispatch(stub_lib *lib, j4_sandbox *sb, int lvo,
                     struct M68KState *st, char *errbuf, unsigned errlen)
{
    (void)sb;
    const j3_lvo_desc *base = lvo_desc(lvo);
    if (!base) { if (errbuf) snprintf(errbuf, errlen, "LVO %d not implemented", lvo); return 1; }

    /* Per-dispatch descriptor copy with this library's base filled in. */
    j3_lvo_desc desc = *base;
    desc.libbase = lib->libbase;

    /* Build (once, then cache) the reverse-H3 marshal thunk via the REAL [J3] marshaller
     * (emitted into a MAP_JIT region), then invoke it: it reads the source 68k registers from
     * the M68KState, places them in AAPCS64 x0..x7, blr's the native stub, and (if the LVO
     * returns) stores the native return into st->d[0]. */
    g_active = lib;
    j3_thunk_fn thunk = cached_thunk(&desc, errbuf, errlen);
    if (!thunk) { g_active = NULL; return 1; }
    thunk(st);
    g_active = NULL;
    return 0;
}
