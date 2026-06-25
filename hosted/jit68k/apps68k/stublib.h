/* stublib.h — a minimal STUB AmigaOS library environment for the 68k JIT harness
 * (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Provides a tiny fake `exec`-like library inside the [J4]
 * sandbox so that a library-calling 68k program (apps68k/libcall.s) can be
 * dispatched through the [J3] negative-offset LVO bridge and have its behaviour
 * OBSERVED — every library call is recorded with its arguments.
 *
 * =========================== GROUNDING (the real AROS contracts) ===============
 * Vector layout — arch/m68k-all/include/aros/cpu.h (upstream tree):
 *   struct JumpVec { unsigned short jmp; void *vec; };                 (lines 50-54)
 *   #define __AROS_ASMJMP        0x4EF9                                (line 61)
 *   #define LIB_VECTSIZE         (sizeof(struct JumpVec)) == 6 on m68k (lines 19,81)
 *   #define __AROS_GETJUMPVEC(lib,n) (&(((struct JumpVec*)(lib))[-(n)])) (line 82)
 * => vector n sits at byte address  libbase - n*6, and on the 68k a caller jumps
 *    there via a 6-byte FULLJMP  `0x4EF9 <abs32>`  (__AROS_USE_FULLJMP, line 64).
 * This stub builds EXACTLY that table in the sandbox (downward from the base), with
 * the 68k-ABI stride 6 — the same value the [J3] bridge uses (J3_M68K_LIB_VECTSIZE).
 * The `<abs32>` target of each vector is a SENTINEL 68k address; the harness never
 * executes the 0x4EF9 itself — it recognises the jsr-through-vector PC, maps it to
 * (libbase, n) via j3_vector_recognise, and invokes the matching [J3] marshal thunk.
 *
 * LVOs (the negative indices; standard AmigaOS exec.library offsets):
 *   AllocMem  LVO 33  (byte offset -198 = -33*6) : d0=byteSize d1=requirements -> d0=ptr
 *   FreeMem   LVO 35  (byte offset -210 = -35*6) : a1=memoryBlock d0=byteSize    (void)
 *   PutChar   LVO  5  (byte offset  -30 =  -5*6) : d0=char                       (void)
 * (PutChar is a stand-in "print"; the real exec LVO 5 is Supervisor — we reuse the
 * slot purely as an observable output sink for the harness, documented as such.)
 * ===============================================================================
 */
#ifndef APPS68K_STUBLIB_H
#define APPS68K_STUBLIB_H

#include <stdint.h>
#include "j4_hunk.h"

/* The standard exec LVO indices the stub library implements (positive n; the byte
 * offset a 68k caller uses is -n*6). Grounded against the classic AmigaOS exec
 * vector numbering. */
#define STUB_LVO_PUTCHAR    5     /* -30  : observable "print" sink              */
#define STUB_LVO_ALLOCMEM  33     /* -198 : AllocMem(d0=size, d1=flags) -> d0    */
#define STUB_LVO_FREEMEM   35     /* -210 : FreeMem(a1=block, d0=size)           */

#define STUB_MEMF_CLEAR     1u    /* requirements flag the program passes        */

/* The call log: every dispatched LVO call appends one record so the harness can
 * assert the exact sequence + arguments a library-calling program produced. */
typedef struct {
    int       lvo;        /* positive LVO index that was called                 */
    uint32_t  arg_d0;     /* d0 at the call (size / char)                        */
    uint32_t  arg_d1;     /* d1 at the call (requirements)                       */
    uint32_t  arg_a1;     /* a1 at the call (memoryBlock)                        */
    uint32_t  ret_d0;     /* value the stub returned into d0 (AllocMem ptr)      */
} stub_call_rec;

#define STUB_MAX_CALLS 64

typedef struct {
    /* The library base, in 68k sandbox space. The jump-vector table grows DOWNWARD
     * from here (vector n at base - n*6). */
    uint32_t       libbase;

    /* The simplest possible sandbox allocator for AllocMem: a bump cursor over a
     * region the harness reserves inside the sandbox. Returns 68k sandbox pointers. */
    uint32_t       heap_base;     /* 68k address of the AllocMem heap            */
    uint32_t       heap_end;      /* one past the heap                           */
    uint32_t       heap_next;     /* bump cursor                                 */

    /* The observable call log. */
    stub_call_rec  calls[STUB_MAX_CALLS];
    int            ncalls;

    /* The "print" sink PutChar writes into (so a program's output is observable). */
    char           out[256];
    int            outlen;

    /* Bytes currently allocated (incremented by AllocMem, decremented by FreeMem)
     * so the harness can assert the program freed what it allocated. */
    uint32_t       bytes_outstanding;
} stub_lib;

/* Build the stub library inside `sb`: place the library base at `libbase`, lay down
 * the negative-offset JumpVec table (0x4EF9 <sentinel> per implemented LVO, stride
 * 6, downward from the base) exactly per cpu.h, and set up the AllocMem heap over
 * [heap_base, heap_end). Returns 0 on success. */
int  stublib_init(stub_lib *lib, j4_sandbox *sb, uint32_t libbase,
                  uint32_t heap_base, uint32_t heap_end);

/* Dispatch one library call: given the 68k machine state at a recognised
 * jsr-through-vector (the LVO index n recovered by j3_vector_recognise) and the
 * sandbox, marshal the source 68k registers into the native stub via the [J3]
 * bridge, run it, store any return into d0, and append a call record. Returns 0 on
 * a recognised+dispatched LVO, nonzero if `lvo` is not implemented. */
struct M68KState;  /* the [J3] state struct (j3_jit68k.h) */
int  stublib_dispatch(stub_lib *lib, j4_sandbox *sb, int lvo,
                      struct M68KState *st, char *errbuf, unsigned errlen);

/* Convenience: the 68k byte-offset (negative) a caller uses to reach LVO n. */
uint32_t stublib_vector_addr(const stub_lib *lib, int lvo);

#endif /* APPS68K_STUBLIB_H */
