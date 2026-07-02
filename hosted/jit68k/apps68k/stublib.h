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

/* ---- the STUB-DOS LVO set (hosted/rust/STD68K-PLAN.md piece 2) ----------------
 * Host-POSIX-backed calls so a 68k C runtime (rust68k/libc68k) can offer the
 * POSIX-ish symbols the Rust std pal uses. OUR numbering (10..21; nothing to be
 * ABI-compatible with — this is the stub library's own vector table). Conventions:
 *   - args in d1/d2/d3 (d0 carries the return);
 *   - returns: >= 0 result, or NEGATIVE errno (NetBSD numbering, which the darwin
 *     host shares for the classic values the Rust pal decodes);
 *   - pointers are 68k sandbox addresses, bounds-checked before host access;
 *   - fds are host fds; 0 reads EOF, 1/2 write to the capture buffer (`out`),
 *     close(<3) is a no-op — the single-task program cannot hurt the harness;
 *   - Stat/FStat write a FIXED 20-byte big-endian record at d2:
 *       u32 kind (0 file, 1 dir, 2 other), u32 size_hi, u32 size_lo,
 *       u32 mtime_secs, u32 readonly;
 *   - GetTime writes u32 secs + u32 nanos at d2 (d1: 0 monotonic, 1 realtime). */
#define STUB_LVO_OPEN      10     /* -60  : Open(d1=path, d2=flags, d3=mode)     */
#define STUB_LVO_CLOSE     11     /* -66  : Close(d1=fd)                         */
#define STUB_LVO_READ      12     /* -72  : Read(d1=fd, d2=buf, d3=len)          */
#define STUB_LVO_WRITE     13     /* -78  : Write(d1=fd, d2=buf, d3=len)         */
#define STUB_LVO_LSEEK     14     /* -84  : LSeek(d1=fd, d2=off(i32), d3=whence) */
#define STUB_LVO_DELETE    15     /* -90  : Delete(d1=path)  (unlink)            */
#define STUB_LVO_MKDIR     16     /* -96  : MkDir(d1=path, d2=mode)              */
#define STUB_LVO_RMDIR     17     /* -102 : RmDir(d1=path)                       */
#define STUB_LVO_STAT      18     /* -108 : Stat(d1=path, d2=rec20)              */
#define STUB_LVO_FSTAT     19     /* -114 : FStat(d1=fd, d2=rec20)               */
#define STUB_LVO_GETTIME   20     /* -120 : GetTime(d1=which, d2=out8)           */
#define STUB_LVO_ENTROPY   21     /* -126 : Entropy(d1=buf, d2=len)              */

/* The open-flags wire values (libc68k passes these; the host stub translates to
 * the host's O_*). Same values the AROS posixc fcntl.h uses, so the Rust pal's
 * encodings pass through libc68k unchanged. */
#define STUB_O_RDONLY   0x0001
#define STUB_O_WRONLY   0x0002
#define STUB_O_RDWR     0x0003
#define STUB_O_CREAT    0x0040
#define STUB_O_EXCL     0x0080
#define STUB_O_TRUNC    0x0200
#define STUB_O_APPEND   0x0400

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

    /* The "print" sink PutChar AND stub-DOS Write(fd 1/2) append into (so a program's
     * output is observable). 1 MiB: std-sized programs print freely; the [J5j]
     * Mandelbrot's 1690 bytes was the old high-water mark. */
    char           out[1 << 20];
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
