// Hosted AArch64 AROS — Phase 2 H12: exec.library boot (the capstone).
//
// The 11 spikes each proved a mechanism; H12 makes them ONE coherent miniature
// exec, assembled the AROS way: exec.library is the hub, and every service is
// reached through its negative-offset LVO jump-vector table (H8) — exactly how
// real AROS works (`AROS_CALL`/`__AROS_GETVECADDR` off SysBase). The services
// behind those vectors are the real ones we built: AllocMem/FreeMem (H5),
// Signal/Wait (H9), AddTask + the priority scheduler (H4/H6). Two tasks are
// AddTask()'d, each AllocMem()'s a buffer and runs a Signal/Wait handshake — all
// via SysBase LVOs, scheduled preemptively off the SIGALRM "timer".
//
// To prove dispatch genuinely routes through the vector table (not direct calls),
// we SetFunction() an instrument over LVO_Signal and confirm its counter tracks
// the handshakes. This is the "it all comes together as a tiny AROS exec on the
// MacBook" demonstration.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

// ===== exec lists ===========================================================
struct Node { struct Node *ln_Succ, *ln_Pred; uint8_t ln_Type; int8_t ln_Pri; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; uint8_t lh_Type; };
static void NewList(struct List *l) { l->lh_Head = (struct Node *)&l->lh_Tail; l->lh_Tail = NULL; l->lh_TailPred = (struct Node *)&l->lh_Head; }
static int IsListEmpty(struct List *l) { return l->lh_TailPred == (struct Node *)l; }
static struct Node *GetHead(struct List *l) { return IsListEmpty(l) ? NULL : l->lh_Head; }
static void Remove(struct Node *n) { n->ln_Pred->ln_Succ = n->ln_Succ; n->ln_Succ->ln_Pred = n->ln_Pred; }
static void Enqueue(struct List *l, struct Node *n) {
    struct Node *p;
    for (p = l->lh_Head; p->ln_Succ; p = p->ln_Succ) if (p->ln_Pri < n->ln_Pri) break;
    n->ln_Pred = p->ln_Pred; n->ln_Succ = p; p->ln_Pred->ln_Succ = n; p->ln_Pred = n;
}

// ===== LVO jump-vector machinery (H8 / x86_64-all cpu.h convention) ==========
struct JumpVec { void *vec; };
#define LIB_VECTSIZE              ((int)sizeof(struct JumpVec))
#define __AROS_GETJUMPVEC(lib, n) (&((struct JumpVec *)(lib))[-(long)(n)])
#define __AROS_GETVECADDR(lib, n) (__AROS_GETJUMPVEC(lib, n)->vec)
#define __AROS_SETVECADDR(lib, n, a) (__AROS_GETJUMPVEC(lib, n)->vec = (a))
static void *_not_impl(void) { return NULL; }
#define __AROS_INITVEC(lib, n)    __AROS_SETVECADDR(lib, n, (void *)_not_impl)
// Call an exec service THROUGH the LVO table (base passed as first arg, AROS-style)
#define CALL_LVO(base, lvo, ftype, ...) ((ftype)__AROS_GETVECADDR((base), (lvo)))((struct ExecBase *)(base), ##__VA_ARGS__)

// ===== Forbid/Permit ========================================================
static volatile sig_atomic_t forbid_cnt = 0;
static void Forbid(void) { forbid_cnt++; __asm__ volatile("" ::: "memory"); }
static void Permit(void) { __asm__ volatile("" ::: "memory"); if (forbid_cnt > 0) forbid_cnt--; }

// ===== memory (H5, compact) =================================================
#define MEMCHUNK_TOTAL 16
#define MEMF_CLEAR (1L << 16)
#define ROUNDUP(x, a) (((uintptr_t)(x) + (a) - 1) & ~(uintptr_t)((a) - 1))
struct MemChunk { struct MemChunk *mc_Next; uintptr_t mc_Bytes; };
struct MemHeader { struct Node mh_Node; uint16_t mh_Attributes; struct MemChunk *mh_First; void *mh_Lower, *mh_Upper; uintptr_t mh_Free; };
static void *mh_Alloc(struct MemHeader *mh, uintptr_t size, unsigned long flags) {
    if (!size) return NULL;
    uintptr_t bs = ROUNDUP(size, MEMCHUNK_TOTAL);
    if (mh->mh_Free < bs) return NULL;
    struct MemChunk *p1 = (struct MemChunk *)&mh->mh_First, *p2 = p1->mc_Next, *mc = NULL;
    while (p2) { if (p2->mc_Bytes >= bs) { mc = p1; break; } p1 = p2; p2 = p1->mc_Next; }
    if (!mc) return NULL;
    p1 = mc; p2 = p1->mc_Next;
    if (p2->mc_Bytes == bs) { p1->mc_Next = p2->mc_Next; mc = p2; }
    else { struct MemChunk *r = (struct MemChunk *)((uint8_t *)p2 + bs); p1->mc_Next = r; r->mc_Next = p2->mc_Next; r->mc_Bytes = p2->mc_Bytes - bs; mc = p2; }
    mh->mh_Free -= bs; if (flags & MEMF_CLEAR) memset(mc, 0, bs); return mc;
}
static int mh_Free_(struct MemHeader *mh, void *addr, uintptr_t size) {
    if (!addr || !size) return 1;
    uintptr_t bs = ROUNDUP(size + ((uintptr_t)addr & (MEMCHUNK_TOTAL - 1)), MEMCHUNK_TOTAL);
    struct MemChunk *p1 = (struct MemChunk *)&mh->mh_First, *p2 = mh->mh_First;
    struct MemChunk *p3 = (struct MemChunk *)((uintptr_t)addr & ~(uintptr_t)(MEMCHUNK_TOTAL - 1));
    uint8_t *p4 = (uint8_t *)p3 + bs;
    if (!p2) { p3->mc_Bytes = bs; p3->mc_Next = NULL; p1->mc_Next = p3; mh->mh_Free += bs; return 1; }
    do { if (p2 >= p3) { if (p4 > (uint8_t *)p2) return 0; break; } p1 = p2; p2 = p2->mc_Next; } while (p2);
    if (p1 != (struct MemChunk *)&mh->mh_First) { if ((uint8_t *)p1 + p1->mc_Bytes > (uint8_t *)p3) return 0; if ((uint8_t *)p1 + p1->mc_Bytes == (uint8_t *)p3) p3 = p1; else p1->mc_Next = p3; }
    else p1->mc_Next = p3;
    if (p4 == (uint8_t *)p2 && p2) { p4 += p2->mc_Bytes; p2 = p2->mc_Next; }
    p3->mc_Next = p2; p3->mc_Bytes = p4 - (uint8_t *)p3; mh->mh_Free += bs; return 1;
}

// ===== Task + ExecBase (the library base) ===================================
#define TS_RUN 2
#define TS_READY 3
#define TS_WAIT 4
struct Library { struct Node lib_Node; uint8_t lib_Flags, lib_pad; uint16_t lib_NegSize, lib_PosSize, lib_Version, lib_Revision; };
struct Task {
    struct Node tc_Node; uint8_t tc_Flags, tc_State; uint32_t tc_SigRecvd, tc_SigWait;
    void *tc_SPReg, *tc_SPLower, *tc_SPUpper; _STRUCT_MCONTEXT64 tc_Context;
    void (*startpc)(struct Task *); long ok;
};
struct ExecBase {
    struct Library    LibNode;            // SysBase IS a library (faithful)
    struct Task      *ThisTask;
    struct List       TaskReady, TaskWait;
    struct MemHeader *MH;
    long              DispCount;
};
static struct ExecBase *SysBase;
#define GET_THIS_TASK    (SysBase->ThisTask)
#define SET_THIS_TASK(t) (SysBase->ThisTask = (t))

// ===== scheduler (H4/H6) ====================================================
#define NWORK 2
#define STK   (1 << 18)
#define TICK_LIMIT 400
static struct Task boot_task;
static struct Task *workers[NWORK];
static volatile sig_atomic_t sched_on = 0, inited = 0, ticks = 0;

static int core_Schedule(void) { return !IsListEmpty(&SysBase->TaskReady); }
static void core_Switch(void) {
    struct Task *t = GET_THIS_TASK;
    if (t->tc_State != TS_RUN) return;
    t->tc_State = TS_READY;
    if (t != &boot_task) Enqueue(&SysBase->TaskReady, &t->tc_Node);
}
static struct Task *core_Dispatch(void) {
    struct Node *n = GetHead(&SysBase->TaskReady);
    if (!n) return NULL;
    Remove(n); struct Task *nt = (struct Task *)n; SysBase->DispCount++; SET_THIS_TASK(nt); nt->tc_State = TS_RUN; return nt;
}
static void cpu_Switch(_STRUCT_MCONTEXT64 *m) { struct Task *t = GET_THIS_TASK; t->tc_Context = *m; t->tc_SPReg = (void *)m->__ss.__sp; core_Switch(); }
static void cpu_Dispatch(_STRUCT_MCONTEXT64 *m) { struct Task *nt = core_Dispatch(); if (nt) *m = nt->tc_Context; }
static void on_alarm(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si; ucontext_t *uc = (ucontext_t *)ucv; _STRUCT_MCONTEXT64 *m = uc->uc_mcontext;
    ticks++;
    if (!sched_on) return;
    if (!inited) {
        boot_task.tc_Context = *m;
        for (int i = 0; i < NWORK; i++) {
            struct Task *w = workers[i];
            w->tc_Context = *m; memset(w->tc_Context.__ss.__x, 0, sizeof w->tc_Context.__ss.__x);
            w->tc_Context.__ss.__pc = (uint64_t)w->startpc; w->tc_Context.__ss.__x[0] = (uint64_t)w;
            w->tc_Context.__ss.__sp = (uint64_t)w->tc_SPReg; w->tc_Context.__ss.__fp = 0; w->tc_Context.__ss.__lr = 0;
        }
        inited = 1; return;
    }
    if (forbid_cnt > 0) return;
    if (ticks >= TICK_LIMIT) { sched_on = 0; SET_THIS_TASK(&boot_task); boot_task.tc_State = TS_RUN; *m = boot_task.tc_Context; return; }
    if (core_Schedule()) { cpu_Switch(m); cpu_Dispatch(m); }
}

// ===== the exec.library SERVICES (reached only through LVOs) =================
// AROS calling convention: library base is the explicit first argument.
static void *exAllocMem(struct ExecBase *eb, uintptr_t size, unsigned long flags) { Forbid(); void *p = mh_Alloc(eb->MH, size, flags); Permit(); return p; }
static void  exFreeMem (struct ExecBase *eb, void *p, uintptr_t size)             { Forbid(); mh_Free_(eb->MH, p, size); Permit(); }
static void  exSignal  (struct ExecBase *eb, struct Task *task, uint32_t sigset) {
    Forbid();
    task->tc_SigRecvd |= sigset;
    if ((task->tc_SigRecvd & task->tc_SigWait) && task->tc_State == TS_WAIT) {
        Remove(&task->tc_Node); task->tc_State = TS_READY; Enqueue(&eb->TaskReady, &task->tc_Node);
    }
    Permit();
}
static uint32_t exWait(struct ExecBase *eb, uint32_t sigset) {
    struct Task *me = eb->ThisTask;
    Forbid();
    if (!(me->tc_SigRecvd & sigset)) {
        me->tc_SigWait = sigset; me->tc_State = TS_WAIT; Enqueue(&eb->TaskWait, &me->tc_Node);
        Permit();
        while (*(volatile uint8_t *)&me->tc_State == TS_WAIT) { }
        Forbid();
    }
    uint32_t rcvd = me->tc_SigRecvd & sigset; me->tc_SigRecvd &= ~sigset; Permit(); return rcvd;
}
static void exAddTask(struct ExecBase *eb, struct Task *t) { t->tc_State = TS_READY; Enqueue(&eb->TaskReady, &t->tc_Node); }

// LVO numbers (illustrative; the dispatch mechanism is what's faithful)
#define LVO_AllocMem 1
#define LVO_FreeMem  2
#define LVO_Signal   3
#define LVO_Wait     4
#define LVO_AddTask  5
#define LVO_MAX      6
typedef void *(*allocmem_t)(struct ExecBase *, uintptr_t, unsigned long);
typedef void  (*freemem_t )(struct ExecBase *, void *, uintptr_t);
typedef void  (*signal_t  )(struct ExecBase *, struct Task *, uint32_t);
typedef uint32_t (*wait_t  )(struct ExecBase *, uint32_t);
typedef void  (*addtask_t )(struct ExecBase *, struct Task *);

// MakeLibrary: AllocMem (neg vectors | pos ExecBase) and install the services.
static struct ExecBase *MakeExecBase(struct MemHeader *mh, void *const *funcs, int nvec, uintptr_t posSize) {
    uintptr_t negSize = (uintptr_t)nvec * LIB_VECTSIZE;
    uint8_t *mem = mh_Alloc(mh, negSize + posSize, MEMF_CLEAR);
    void *base = mem + negSize;
    for (int n = 1; n <= nvec; n++) { __AROS_INITVEC(base, n); if (funcs[n - 1]) __AROS_SETVECADDR(base, n, funcs[n - 1]); }
    struct ExecBase *eb = (struct ExecBase *)base;
    eb->LibNode.lib_Node.ln_Name = "exec.library"; eb->LibNode.lib_Version = 50;
    eb->LibNode.lib_NegSize = (uint16_t)negSize; eb->LibNode.lib_PosSize = (uint16_t)posSize;
    return eb;
}
static void *SetFunction(struct ExecBase *base, long byteOffset, void *newfn) {
    long lvo = (-byteOffset) / LIB_VECTSIZE;
    void *old = __AROS_GETVECADDR(base, lvo); __AROS_SETVECADDR(base, lvo, newfn); return old;
}

// ===== the demo tasks: everything via SysBase LVOs ==========================
#define SIG_PING (1u << 0)
#define SIG_PONG (1u << 1)
static struct Task *t_a, *t_b;
static volatile long g_sig_calls;                  // counted by the SetFunction instrument
static signal_t orig_Signal;
static void instr_Signal(struct ExecBase *eb, struct Task *t, uint32_t s) { g_sig_calls++; orig_Signal(eb, t, s); }

// task A: AllocMem a buffer via LVO, then ping-pong with B via LVO Signal/Wait.
static void task_a(struct Task *self) {
    unsigned char *buf = CALL_LVO(SysBase, LVO_AllocMem, allocmem_t, 256, MEMF_CLEAR);
    if (buf) { memset(buf, 0xA5, 256); self->ok = (buf[0] == 0xA5 && buf[255] == 0xA5); }
    for (;;) {
        CALL_LVO(SysBase, LVO_Signal, signal_t, t_b, SIG_PING);
        CALL_LVO(SysBase, LVO_Wait, wait_t, SIG_PONG);
        self->ok++;
    }
}
static void task_b(struct Task *self) {
    unsigned char *buf = CALL_LVO(SysBase, LVO_AllocMem, allocmem_t, 256, MEMF_CLEAR);
    if (buf) { self->ok = 1; CALL_LVO(SysBase, LVO_FreeMem, freemem_t, buf, 256); }
    for (;;) {
        CALL_LVO(SysBase, LVO_Wait, wait_t, SIG_PING);
        CALL_LVO(SysBase, LVO_Signal, signal_t, t_a, SIG_PONG);
        self->ok++;
    }
}

static struct Task *make_task(const char *name, void (*pc)(struct Task *)) {
    static struct Task pool[NWORK]; static int np;
    struct Task *t = &pool[np++];
    memset(t, 0, sizeof *t);
    t->tc_Node.ln_Name = (char *)name; t->tc_Node.ln_Pri = 0; t->startpc = pc;
    void *stk = mmap(NULL, STK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    t->tc_SPLower = stk; t->tc_SPUpper = (uint8_t *)stk + STK; t->tc_SPReg = (uint8_t *)stk + STK;
    return t;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H12a] exec.library boot: the full exec, reached through the LVO hub\n");

    // "RAM" from macOS, then a MemHeader over it — the heap exec.library lives in.
    const uintptr_t LEN = 8u << 20;
    void *region = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    static struct MemHeader mh;
    uintptr_t l = LEN & ~(uintptr_t)(MEMCHUNK_TOTAL - 1);
    mh.mh_Node.ln_Type = 0; mh.mh_Lower = region; mh.mh_Upper = (uint8_t *)region + l;
    mh.mh_First = (struct MemChunk *)region; mh.mh_First->mc_Next = NULL; mh.mh_First->mc_Bytes = l; mh.mh_Free = l;

    // Stand exec.library up: services installed into the LVO table below the base.
    void *const funcs[LVO_MAX] = { (void *)exAllocMem, (void *)exFreeMem, (void *)exSignal, (void *)exWait, (void *)exAddTask, NULL };
    SysBase = MakeExecBase(&mh, funcs, LVO_MAX, sizeof(struct ExecBase));
    SysBase->MH = &mh;
    NewList(&SysBase->TaskReady); NewList(&SysBase->TaskWait);
    printf("[H12]   exec.library v%d up (negSize=%d), services live at LVOs 1..%d\n",
           SysBase->LibNode.lib_Version, SysBase->LibNode.lib_NegSize, LVO_MAX - 1);

    // Instrument LVO_Signal via SetFunction to PROVE calls route through the table.
    orig_Signal = (signal_t)SetFunction(SysBase, -LVO_Signal * LIB_VECTSIZE, (void *)instr_Signal);

    // boot anchor
    memset(&boot_task, 0, sizeof boot_task);
    boot_task.tc_Node.ln_Name = "boot"; boot_task.tc_Node.ln_Pri = -128; boot_task.tc_State = TS_RUN;
    SET_THIS_TASK(&boot_task);

    // AddTask the two workers THROUGH the LVO.
    t_a = make_task("A", task_a); t_b = make_task("B", task_b);
    workers[0] = t_a; workers[1] = t_b;
    CALL_LVO(SysBase, LVO_AddTask, addtask_t, t_a);
    CALL_LVO(SysBase, LVO_AddTask, addtask_t, t_b);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_alarm; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 5000; it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);

    sched_on = 1;
    while (ticks < TICK_LIMIT) ;
    struct itimerval offv; memset(&offv, 0, sizeof offv); setitimer(ITIMER_REAL, &offv, NULL);

    long a = t_a->ok, b = t_b->ok, sigs = g_sig_calls;
    int ok = 1;
    if (__AROS_GETVECADDR(SysBase, LVO_AllocMem) != (void *)exAllocMem) { printf("[H12] FAIL: AllocMem vector wrong\n"); ok = 0; }
    if (a < 10 || b < 10) { printf("[H12] FAIL: tasks didn't run via LVO (a=%ld b=%ld)\n", a, b); ok = 0; }
    if (sigs < 10) { printf("[H12] FAIL: SetFunction instrument saw too few Signal LVO calls (%ld)\n", sigs); ok = 0; }

    printf("[H12]   tasks ran via LVO services: A.ok=%ld B.ok=%ld (AllocMem+Signal/Wait through the base)\n", a, b);
    printf("[H12]   SetFunction-instrumented LVO_Signal observed %ld dispatches -> calls really go through the table\n", sigs);
    printf("[H12]   dispatched=%ld over %d ticks; heap free=%lu\n", SysBase->DispCount, (int)ticks, (unsigned long)mh.mh_Free);
    if (ok)
        printf("[H12] exec.library boot ok: a tiny AROS exec runs on macOS with every service via the LVO hub\n");
    else
        printf("[H12] FAIL: see checks above\n");
    munmap(region, LEN);
    return ok ? 0 : 1;
}
