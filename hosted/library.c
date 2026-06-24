// Hosted AArch64 AROS — Phase 2 H8: a tiny exec.library via the real LVO mechanism.
//
// Everything in AROS is a library: you reach a function through a jump-vector
// table held just BELOW the library base, indexed by a negative LVO (Library
// Vector Offset). H8 stands a minimal exec.library up that way, hosted, and
// exercises the three pieces that make AROS modular:
//   - MakeLibrary/MakeFunctions: build the JumpVec table below the base;
//   - the LVO call: dispatch indirectly through __AROS_GETVECADDR(base, lvo);
//   - SetFunction: hot-patch a vector at runtime (the AROS hooking mechanism).
//
// Grounded: on 64-bit native AROS the vector table is "only pointers, no jump
// code" (arch/x86_64-all/include/aros/cpu.h, verbatim struct JumpVec below) —
// __AROS_USE_FULLJMP is OFF. That's the key hosted result: AArch64 follows the
// same convention, so libraries are plain function pointers + indirect calls,
// with NO runtime code generation — so NO Apple-Silicon W^X / MAP_JIT wall. This
// spike proves that live. (m68k/ARM32 use executable FULLJMP vectors and WOULD
// hit W^X; 64-bit native does not.)
//
// SetFunction's contract is grounded against rom/exec/setfunction.c: it takes the
// negative byte offset (-LVO * LIB_VECTSIZE), Forbid()s, returns the old vector.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

// ===== exec/cpu.h vector convention (verbatim from x86_64-all) ==============
struct JumpVec { void *vec; };
#define LIB_VECTSIZE              ((int)sizeof(struct JumpVec))
#define __AROS_GETJUMPVEC(lib, n) (&((struct JumpVec *)(lib))[-(long)(n)])
#define __AROS_GETVECADDR(lib, n) (__AROS_GETJUMPVEC(lib, n)->vec)
#define __AROS_SETVECADDR(lib, n, addr) (__AROS_GETJUMPVEC(lib, n)->vec = (addr))
static void *_aros_not_implemented(void);     // fwd
#define __AROS_INITVEC(lib, n)    __AROS_SETVECADDR(lib, n, (void *)_aros_not_implemented)

// ===== exec/nodes.h + memory + library headers ==============================
#define NT_MEMORY  10
#define NT_LIBRARY 9
#define LIBF_CHANGED (1 << 1)
struct Node { struct Node *ln_Succ, *ln_Pred; uint8_t ln_Type; int8_t ln_Pri; char *ln_Name; };
struct MemChunk { struct MemChunk *mc_Next; uintptr_t mc_Bytes; };
struct MemHeader { struct Node mh_Node; uint16_t mh_Attributes; struct MemChunk *mh_First; void *mh_Lower, *mh_Upper; uintptr_t mh_Free; };
struct Library {
    struct Node lib_Node;
    uint8_t  lib_Flags, lib_pad;
    uint16_t lib_NegSize, lib_PosSize, lib_Version, lib_Revision;
    uint16_t lib_OpenCnt;
};

// ===== H5 allocator (compact) ===============================================
#define MEMCHUNK_TOTAL 16
#define MEMF_CLEAR (1L << 16)
#define ROUNDUP(x, a) (((uintptr_t)(x) + (a) - 1) & ~(uintptr_t)((a) - 1))
static struct MemHeader sysmh;
static void *Allocate(struct MemHeader *mh, uintptr_t size, unsigned long flags) {
    if (!size) return NULL;
    uintptr_t bs = ROUNDUP(size, MEMCHUNK_TOTAL);
    if (mh->mh_Free < bs) return NULL;
    struct MemChunk *p1 = (struct MemChunk *)&mh->mh_First, *p2 = p1->mc_Next, *mc = NULL;
    while (p2) { if (p2->mc_Bytes >= bs) { mc = p1; break; } p1 = p2; p2 = p1->mc_Next; }
    if (!mc) return NULL;
    p1 = mc; p2 = p1->mc_Next;
    if (p2->mc_Bytes == bs) { p1->mc_Next = p2->mc_Next; mc = p2; }
    else { struct MemChunk *r = (struct MemChunk *)((uint8_t *)p2 + bs); p1->mc_Next = r; r->mc_Next = p2->mc_Next; r->mc_Bytes = p2->mc_Bytes - bs; mc = p2; }
    mh->mh_Free -= bs;
    if (flags & MEMF_CLEAR) memset(mc, 0, bs);
    return mc;
}
static int Deallocate(struct MemHeader *mh, void *addr, uintptr_t size) {
    if (!addr || !size) return 1;
    uintptr_t bs = ROUNDUP(size + ((uintptr_t)addr & (MEMCHUNK_TOTAL - 1)), MEMCHUNK_TOTAL);
    struct MemChunk *p1 = (struct MemChunk *)&mh->mh_First, *p2 = mh->mh_First;
    struct MemChunk *p3 = (struct MemChunk *)((uintptr_t)addr & ~(uintptr_t)(MEMCHUNK_TOTAL - 1));
    uint8_t *p4 = (uint8_t *)p3 + bs;
    if (!p2) { p3->mc_Bytes = bs; p3->mc_Next = NULL; p1->mc_Next = p3; mh->mh_Free += bs; return 1; }
    do { if (p2 >= p3) { if (p4 > (uint8_t *)p2) return 0; break; } p1 = p2; p2 = p2->mc_Next; } while (p2);
    if (p1 != (struct MemChunk *)&mh->mh_First) {
        if ((uint8_t *)p1 + p1->mc_Bytes > (uint8_t *)p3) return 0;
        if ((uint8_t *)p1 + p1->mc_Bytes == (uint8_t *)p3) p3 = p1; else p1->mc_Next = p3;
    } else p1->mc_Next = p3;
    if (p4 == (uint8_t *)p2 && p2) { p4 += p2->mc_Bytes; p2 = p2->mc_Next; }
    p3->mc_Next = p2; p3->mc_Bytes = p4 - (uint8_t *)p3; mh->mh_Free += bs;
    return 1;
}
static void mh_Init(struct MemHeader *mh, void *base, uintptr_t len) {
    len &= ~(uintptr_t)(MEMCHUNK_TOTAL - 1);
    mh->mh_Node.ln_Type = NT_MEMORY; mh->mh_Node.ln_Name = "hosted-ram";
    mh->mh_Lower = base; mh->mh_Upper = (uint8_t *)base + len;
    mh->mh_First = (struct MemChunk *)base; mh->mh_First->mc_Next = NULL; mh->mh_First->mc_Bytes = len; mh->mh_Free = len;
}

// ===== Forbid/Permit (SetFunction arbitrates with it) =======================
static int forbid_cnt = 0;
static void Forbid(void) { forbid_cnt++; }
static void Permit(void) { if (forbid_cnt > 0) forbid_cnt--; }

// ===== our exec.library: the base + its functions ===========================
struct ExecBase {
    struct Library    LibNode;        // SysBase starts with a Library node (faithful)
    struct MemHeader *eb_MH;
};
static void *_aros_not_implemented(void) { fprintf(stderr, "[exec] called an unimplemented LVO\n"); return NULL; }

// exec functions, AROS-style: the library base is the (explicit) first argument.
static void *exAllocMem(struct ExecBase *eb, uintptr_t size, unsigned long flags) { return Allocate(eb->eb_MH, size, flags); }
static int   exFreeMem (struct ExecBase *eb, void *p, uintptr_t size)             { return Deallocate(eb->eb_MH, p, size); }
static uintptr_t exAvailMem(struct ExecBase *eb)                                  { return eb->eb_MH->mh_Free; }

typedef void *(*allocmem_t)(struct ExecBase *, uintptr_t, unsigned long);
typedef int   (*freemem_t )(struct ExecBase *, void *, uintptr_t);
typedef uintptr_t (*availmem_t)(struct ExecBase *);
// Dispatch THROUGH the LVO table (never call the impl directly).
#define CALL_LVO(base, lvo, ftype, ...) ((ftype)__AROS_GETVECADDR((base), (lvo)))((struct ExecBase *)(base), ##__VA_ARGS__)

// LVO numbers (illustrative; real exec AllocMem is 33, FreeMem 35 — the mechanism
// is identical, only the indices differ).
#define LVO_AllocMem 1
#define LVO_FreeMem  2
#define LVO_AvailMem 3
#define LVO_MAX      4               // 4th vector left un-implemented on purpose

// MakeLibrary + MakeFunctions: AllocMem the (neg vectors | pos struct) block and
// install the vectors below the base. funcArray[i] -> vector (i+1); NULL leaves
// the not-implemented stub.
static struct ExecBase *MakeLibrary(void *const *funcArray, int vectorCount, uintptr_t posSize) {
    uintptr_t negSize = (uintptr_t)vectorCount * LIB_VECTSIZE;
    uint8_t *mem = Allocate(&sysmh, negSize + posSize, MEMF_CLEAR);
    if (!mem) return NULL;
    void *base = mem + negSize;                          // base sits past the vectors
    for (int n = 1; n <= vectorCount; n++) {
        __AROS_INITVEC(base, n);                        // default: not implemented
        if (funcArray[n - 1])
            __AROS_SETVECADDR(base, n, funcArray[n - 1]);
    }
    struct ExecBase *eb = (struct ExecBase *)base;
    eb->LibNode.lib_Node.ln_Type = NT_LIBRARY;
    eb->LibNode.lib_Node.ln_Name = "exec.library";
    eb->LibNode.lib_NegSize = (uint16_t)negSize;
    eb->LibNode.lib_PosSize = (uint16_t)posSize;
    eb->LibNode.lib_Version = 50;
    return eb;
}

// SetFunction(library, funcOffset=-LVO*LIB_VECTSIZE, newFunc) -> old vector.
static void *SetFunction(struct ExecBase *base, long funcOffset, void *newFunc) {
    long lvo = (-funcOffset) / LIB_VECTSIZE;            // recover the LVO (grounded)
    Forbid();
    base->LibNode.lib_Flags |= LIBF_CHANGED;
    void *old = __AROS_GETVECADDR(base, lvo);
    __AROS_SETVECADDR(base, lvo, newFunc);
    Permit();
    return old;                                          // (SumLibrary checksum skipped — noted)
}

// A SetFunction patch: count AllocMem calls, then delegate to the original.
static allocmem_t orig_AllocMem;
static long patched_calls;
static void *wrapAllocMem(struct ExecBase *eb, uintptr_t size, unsigned long flags) {
    patched_calls++;
    return orig_AllocMem(eb, size, flags);
}

#define CHECK(c) do { if (!(c)) { printf("[H8] FAIL: %s (line %d)\n", #c, __LINE__); ok = 0; } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H8a] hosted exec.library: JumpVec table + LVO calls + SetFunction (no W^X needed)\n");

    const uintptr_t LEN = 8u << 20;
    void *region = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) { printf("[H8] FAIL: mmap\n"); return 1; }
    mh_Init(&sysmh, region, LEN);
    int ok = 1;

    // Build exec.library: 3 functions installed, a 4th vector left un-implemented.
    void *const funcs[LVO_MAX] = { (void *)exAllocMem, (void *)exFreeMem, (void *)exAvailMem, NULL };
    struct ExecBase *SysBase = MakeLibrary(funcs, LVO_MAX, sizeof(struct ExecBase));
    CHECK(SysBase != NULL);
    SysBase->eb_MH = &sysmh;
    CHECK(SysBase->LibNode.lib_Node.ln_Type == NT_LIBRARY);
    // Vectors live just below the base; the un-installed one is the stub.
    CHECK(__AROS_GETVECADDR(SysBase, LVO_AllocMem) == (void *)exAllocMem);
    CHECK(__AROS_GETVECADDR(SysBase, LVO_MAX) == (void *)_aros_not_implemented);
    printf("[H8]   exec.library v%d up: negSize=%d posSize=%d, %d LVOs\n",
           SysBase->LibNode.lib_Version, SysBase->LibNode.lib_NegSize, SysBase->LibNode.lib_PosSize, LVO_MAX);

    // Call AllocMem/AvailMem/FreeMem THROUGH the LVO table (indirect dispatch).
    uintptr_t before = CALL_LVO(SysBase, LVO_AvailMem, availmem_t);
    void *p = CALL_LVO(SysBase, LVO_AllocMem, allocmem_t, 4096, MEMF_CLEAR);
    CHECK(p != NULL);
    memset(p, 0x5A, 4096);
    CHECK(((uint8_t *)p)[0] == 0x5A && ((uint8_t *)p)[4095] == 0x5A);
    CHECK(CALL_LVO(SysBase, LVO_AvailMem, availmem_t) == before - 4096);
    printf("[H8]   LVO call: AllocMem(4096) -> %p, AvailMem %lu -> %lu\n",
           p, (unsigned long)before, (unsigned long)CALL_LVO(SysBase, LVO_AvailMem, availmem_t));

    // SetFunction: hot-patch AllocMem with a counting wrapper, then call again.
    orig_AllocMem = (allocmem_t)SetFunction(SysBase, -LVO_AllocMem * LIB_VECTSIZE, (void *)wrapAllocMem);
    CHECK(orig_AllocMem == (allocmem_t)exAllocMem);                  // returned the old vector
    CHECK(__AROS_GETVECADDR(SysBase, LVO_AllocMem) == (void *)wrapAllocMem);
    void *q = CALL_LVO(SysBase, LVO_AllocMem, allocmem_t, 1024, MEMF_CLEAR);
    CHECK(q != NULL);
    CHECK(patched_calls == 1);                                       // the patch ran
    printf("[H8]   SetFunction: AllocMem patched; next LVO call routed via wrapper (calls=%ld)\n", patched_calls);

    // Free both through the LVO and confirm the heap recovers.
    CALL_LVO(SysBase, LVO_FreeMem, freemem_t, p, 4096);
    CALL_LVO(SysBase, LVO_FreeMem, freemem_t, q, 1024);
    CHECK(CALL_LVO(SysBase, LVO_AvailMem, availmem_t) == before);

    if (ok)
        printf("[H8] hosted exec.library ok: LVO dispatch + SetFunction work hosted; data-pointer vectors, no W^X\n");
    else
        printf("[H8] FAIL: see checks above\n");
    munmap(region, LEN);
    return ok ? 0 : 1;
}
