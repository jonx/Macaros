// Hosted AArch64 AROS — Phase 2 H6: a tiny hosted exec (H4 + H5 composed).
//
// The isolated spikes proved each subsystem; H6 proves they COMPOSE, which is
// where integration bugs hide. One macOS process where:
//   - memory is the H5 MemHeader/MemChunk allocator over one mmap'd region;
//   - tasks (struct Task + stack) are AllocMem'd FROM that heap, not statics;
//   - the H4 priority scheduler preempts them off the SIGALRM "timer IRQ";
//   - Forbid()/Permit() (AROS's dispatch-disable) make AllocMem task-safe, so a
//     preemption landing mid-allocator can't corrupt the free list.
//
// Stress: every worker continuously AllocMem's a scratch block, writes its own
// distinct byte pattern, runs (preemptibly), verifies the pattern, and FreeMem's
// it — under Forbid/Permit around the allocator calls only. If the allocator
// metadata were corrupted by a mid-AllocMem preemption, or two tasks' blocks
// overlapped, the free-list invariants or the patterns would break. They don't.
//
// Grounded pieces are unchanged from H4 (kernel_scheduler.c/kernel_cpu.c shapes)
// and H5 (rom/exec/memory.c stdAlloc/stdDealloc). Forbid/Permit model AROS's
// TDNestCnt-based dispatch disable (single underlying thread ⇒ a non-zero count
// reliably means "don't switch").

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

// ============================ exec lists ====================================
struct Node { struct Node *ln_Succ, *ln_Pred; uint8_t ln_Type; int8_t ln_Pri; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; uint8_t lh_Type; };

static void NewList(struct List *l) {
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}
static int IsListEmpty(struct List *l) { return l->lh_TailPred == (struct Node *)l; }
static struct Node *GetHead(struct List *l) { return IsListEmpty(l) ? NULL : l->lh_Head; }
static void Remove(struct Node *n) { n->ln_Pred->ln_Succ = n->ln_Succ; n->ln_Succ->ln_Pred = n->ln_Pred; }
static void Enqueue(struct List *l, struct Node *n) {
    struct Node *p;
    for (p = l->lh_Head; p->ln_Succ; p = p->ln_Succ)
        if (p->ln_Pri < n->ln_Pri) break;
    n->ln_Pred = p->ln_Pred; n->ln_Succ = p;
    p->ln_Pred->ln_Succ = n;  p->ln_Pred = n;
}

// ============================ H5 allocator ==================================
#define NT_MEMORY 10
struct MemChunk { struct MemChunk *mc_Next; uintptr_t mc_Bytes; };
struct MemHeader {
    struct Node mh_Node; uint16_t mh_Attributes;
    struct MemChunk *mh_First; void *mh_Lower, *mh_Upper; uintptr_t mh_Free;
};
#define MEMCHUNK_TOTAL 16
#define MEMF_ANY    0L
#define MEMF_CLEAR  (1L << 16)
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
    else {
        struct MemChunk *rem = (struct MemChunk *)((uint8_t *)p2 + bs);
        p1->mc_Next = rem; rem->mc_Next = p2->mc_Next; rem->mc_Bytes = p2->mc_Bytes - bs; mc = p2;
    }
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
    do {
        if (p2 >= p3) { if (p4 > (uint8_t *)p2) return 0; break; }
        p1 = p2; p2 = p2->mc_Next;
    } while (p2);
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
    mh->mh_Attributes = MEMF_ANY; mh->mh_Lower = base; mh->mh_Upper = (uint8_t *)base + len;
    mh->mh_First = (struct MemChunk *)base;
    mh->mh_First->mc_Next = NULL; mh->mh_First->mc_Bytes = len; mh->mh_Free = len;
}
static int chunk_count(struct MemHeader *mh) { int n=0; for (struct MemChunk *c=mh->mh_First;c;c=c->mc_Next) n++; return n; }
static uintptr_t free_sum(struct MemHeader *mh) { uintptr_t s=0; for (struct MemChunk *c=mh->mh_First;c;c=c->mc_Next) s+=c->mc_Bytes; return s; }
static int freelist_sane(struct MemHeader *mh) {
    uint8_t *pe = (uint8_t *)mh->mh_Lower;
    for (struct MemChunk *c = mh->mh_First; c; c = c->mc_Next) {
        if ((void *)c < mh->mh_Lower || (uint8_t *)c + c->mc_Bytes > (uint8_t *)mh->mh_Upper) return 0;
        if ((uint8_t *)c < pe) return 0;
        pe = (uint8_t *)c + c->mc_Bytes;
    }
    return 1;
}

// ===================== Forbid/Permit (dispatch disable) =====================
static volatile sig_atomic_t forbid_cnt = 0;
// The compiler barriers are load-bearing: forbid_cnt is volatile but the
// allocator's memory is NOT, and C only orders volatile-to-volatile. Without the
// barriers, -O2 hoists/sinks the free-list writes OUTSIDE this window, where a
// SIGALRM catches them half-done -> free-list corruption. (Single underlying
// thread ⇒ a compiler barrier suffices; no CPU fence needed for same-core signal
// delivery.) This bug is exactly why a hosted port must get Forbid right.
static void Forbid(void) { forbid_cnt++; __asm__ volatile("" ::: "memory"); }
static void Permit(void) { __asm__ volatile("" ::: "memory"); if (forbid_cnt > 0) forbid_cnt--; }
// Allocator wrappers made task-safe by deferring preemption across the call.
static void *AllocMem(uintptr_t size, unsigned long flags) { Forbid(); void *p = Allocate(&sysmh, size, flags); Permit(); return p; }
static void  FreeMem (void *p, uintptr_t size)             { Forbid(); Deallocate(&sysmh, p, size); Permit(); }

// ============================ H4 scheduler ==================================
#define TS_RUN 2
#define TS_READY 3
#define TS_WAIT 4
struct Task {
    struct Node tc_Node; uint8_t tc_Flags, tc_State;
    void *tc_SPReg, *tc_SPLower, *tc_SPUpper;
    _STRUCT_MCONTEXT64 tc_Context;
    void (*startpc)(struct Task *);
    void *stackbase;
    long ran, good; uint8_t mark;
};
struct ExecBase { struct Task *ThisTask; struct List TaskReady; long DispCount; };
static struct ExecBase exb, *SysBase = &exb;
#define GET_THIS_TASK    (SysBase->ThisTask)
#define SET_THIS_TASK(t) (SysBase->ThisTask = (t))

#define NWORK 4
#define STK   (1 << 18)
#define TICK_LIMIT 300
static struct Task boot_task;
static struct Task *workers[NWORK];
static volatile sig_atomic_t sched_on = 0, inited = 0, ticks = 0;

static int core_Schedule(void) { return !IsListEmpty(&SysBase->TaskReady); }
static void core_Switch(void) {
    struct Task *t = GET_THIS_TASK;
    if (t->tc_State != TS_RUN) return;
    if (t != &boot_task && (t->tc_SPReg <= t->tc_SPLower || t->tc_SPReg > t->tc_SPUpper)) { t->tc_State = TS_WAIT; return; }
    t->tc_State = TS_READY;
    if (t != &boot_task) Enqueue(&SysBase->TaskReady, &t->tc_Node);
}
static struct Task *core_Dispatch(void) {
    struct Node *n = GetHead(&SysBase->TaskReady);
    if (!n) return NULL;
    Remove(n);
    struct Task *nt = (struct Task *)n;
    SysBase->DispCount++; SET_THIS_TASK(nt); nt->tc_State = TS_RUN;
    return nt;
}
static void cpu_Switch(_STRUCT_MCONTEXT64 *m) {
    struct Task *t = GET_THIS_TASK;
    t->tc_Context = *m; t->tc_SPReg = (void *)m->__ss.__sp; core_Switch();
}
static void cpu_Dispatch(_STRUCT_MCONTEXT64 *m) {
    struct Task *nt = core_Dispatch();
    if (nt) *m = nt->tc_Context;
}
static void core_ExitInterrupt(_STRUCT_MCONTEXT64 *m) { if (core_Schedule()) { cpu_Switch(m); cpu_Dispatch(m); } }

static void on_alarm(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    _STRUCT_MCONTEXT64 *m = uc->uc_mcontext;
    ticks++;
    if (!sched_on) return;
    if (!inited) {
        boot_task.tc_Context = *m;
        for (int i = 0; i < NWORK; i++) {
            struct Task *w = workers[i];
            w->tc_Context = *m;
            memset(w->tc_Context.__ss.__x, 0, sizeof w->tc_Context.__ss.__x);
            w->tc_Context.__ss.__pc = (uint64_t)w->startpc;
            w->tc_Context.__ss.__x[0] = (uint64_t)w;          // arg: the Task* (AAPCS64 x0)
            w->tc_Context.__ss.__sp = (uint64_t)w->tc_SPReg;
            w->tc_Context.__ss.__fp = 0; w->tc_Context.__ss.__lr = 0;
        }
        inited = 1;
        return;
    }
    // Forbid gates BOTH the normal switch AND shutdown: never interrupt a task
    // that is mid-critical-section (e.g. inside the allocator), or we'd snapshot
    // a half-updated free list. A running task is the only one that can hold
    // forbid_cnt>0 (a parked task was switched out at a forbid==0 point), and it
    // Permits within microseconds, so a clean tick always arrives.
    if (forbid_cnt > 0) return;
    if (ticks >= TICK_LIMIT) {
        sched_on = 0; SET_THIS_TASK(&boot_task); boot_task.tc_State = TS_RUN; *m = boot_task.tc_Context;
        return;
    }
    core_ExitInterrupt(m);
}

// ============================ workers =======================================
// Each worker: AllocMem a scratch block, stamp its own mark, run (preemptible),
// verify the mark survived, FreeMem it. Distinct marks ⇒ a cross-task overlap or
// an allocator corruption shows up as a failed verify or a broken invariant.
static void body(struct Task *self) {
    for (;;) {
        self->ran++;
        void *p = AllocMem(256, MEMF_CLEAR);
        if (p) {
            memset(p, self->mark, 256);
            for (volatile long i = 0; i < 150000; i++) { }   // preemptible work
            int okp = 1;
            for (int i = 0; i < 256; i++) if (((uint8_t *)p)[i] != self->mark) okp = 0;
            if (okp) self->good++;
            FreeMem(p, 256);
        } else {
            for (volatile long i = 0; i < 150000; i++) { }
        }
    }
}

static struct Task *spawn(const char *name, int pri, uint8_t mark) {
    struct Task *t = AllocMem(sizeof *t, MEMF_CLEAR);       // task struct FROM the heap
    void *stk = AllocMem(STK, MEMF_ANY);                    // stack FROM the heap
    t->tc_Node.ln_Name = (char *)name; t->tc_Node.ln_Pri = pri;
    t->startpc = body; t->mark = mark;
    t->stackbase = stk;
    t->tc_SPLower = stk; t->tc_SPUpper = (uint8_t *)stk + STK; t->tc_SPReg = (uint8_t *)stk + STK;
    t->tc_State = TS_READY;
    Enqueue(&SysBase->TaskReady, &t->tc_Node);
    return t;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H6a] hosted exec: tasks AllocMem'd from the H5 heap, scheduled by H4, Forbid-safe\n");

    const uintptr_t LEN = 16u << 20;
    void *region = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) { printf("[H6] FAIL: mmap\n"); return 1; }
    mh_Init(&sysmh, region, LEN);

    NewList(&SysBase->TaskReady);
    memset(&boot_task, 0, sizeof boot_task);
    boot_task.tc_Node.ln_Name = "boot"; boot_task.tc_Node.ln_Pri = -128; boot_task.tc_State = TS_RUN;
    SET_THIS_TASK(&boot_task);

    for (int i = 0; i < NWORK; i++)
        workers[i] = spawn("worker", 0, (uint8_t)(0x40 + i));   // equal priority, distinct marks
    const uintptr_t after_spawn_free = sysmh.mh_Free;

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_alarm; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 10000; it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);

    sched_on = 1;
    while (ticks < TICK_LIMIT) ;        // boot anchor parks after 1st switch; restored at the end
    struct itimerval off; memset(&off, 0, sizeof off); setitimer(ITIMER_REAL, &off, NULL);

    int ok = 1;
    long tot_ran = 0, tot_good = 0;
    for (int i = 0; i < NWORK; i++) {
        struct Task *w = workers[i];
        tot_ran += w->ran; tot_good += w->good;
        if (w->ran < 30) { printf("[H6] FAIL: worker %d starved (ran=%ld)\n", i, w->ran); ok = 0; }
        if (w->good < w->ran - 2) { printf("[H6] FAIL: worker %d pattern broke (good=%ld ran=%ld)\n", i, w->good, w->ran); ok = 0; }
    }
    // Heap must still be consistent after thousands of alloc/free under preemption.
    if (!freelist_sane(&sysmh)) { printf("[H6] FAIL: free list corrupt\n"); ok = 0; }
    if (free_sum(&sysmh) != sysmh.mh_Free) {
        printf("[H6] FAIL: free accounting off: free_sum=%lu mh_Free=%lu (diff=%ld)\n",
               (unsigned long)free_sum(&sysmh), (unsigned long)sysmh.mh_Free,
               (long)free_sum(&sysmh) - (long)sysmh.mh_Free);
        ok = 0;
    }
    // Workers balance alloc/free, so the heap should be back to the post-spawn level
    // (any in-flight scratch at shutdown is at most one block per worker).
    if (sysmh.mh_Free > after_spawn_free) { printf("[H6] FAIL: free > baseline (leak accounting)\n"); ok = 0; }

    printf("[H6]   dispatched=%ld over %d ticks; workers ran=%ld good=%ld\n",
           SysBase->DispCount, (int)ticks, tot_ran, tot_good);
    printf("[H6]   per-worker ran: %ld %ld %ld %ld\n", workers[0]->ran, workers[1]->ran, workers[2]->ran, workers[3]->ran);
    printf("[H6]   heap: free=%lu/%lu chunks=%d sane=%d\n",
           (unsigned long)sysmh.mh_Free, (unsigned long)after_spawn_free, chunk_count(&sysmh), freelist_sane(&sysmh));

    if (ok)
        printf("[H6] hosted exec ok: heap-allocated tasks scheduled preemptively, allocator stayed consistent under Forbid\n");
    else
        printf("[H6] FAIL: see checks above\n");
    munmap(region, LEN);
    return ok ? 0 : 1;
}
