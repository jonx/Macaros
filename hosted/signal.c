// Hosted AArch64 AROS — Phase 2 H9: exec Wait()/Signal() — tasks that BLOCK.
//
// H4/H6 gave a preemptive scheduler, but its tasks only ever round-robin. A real
// exec needs tasks that *block* on signals and *wake* — the heart of every AROS
// device/message/semaphore wait. H9 adds the genuine AROS Signal/Wait state
// machine on top of the H6 kernel, grounded against rom/exec/{wait,signal}.c:
//
//   Wait(sigset):  Forbid; if none of sigset received -> tc_SigWait=sigset,
//                  tc_State=TS_WAIT, Enqueue(TaskWait), give up the CPU; on wake,
//                  return (and clear) the received bits of sigset.
//   Signal(t,sig): t->tc_SigRecvd |= sig; if t was TS_WAIT on these bits ->
//                  Remove from TaskWait, TS_READY, Enqueue(TaskReady).
//
// A blocked task is parked off the ready list, so the scheduler simply never
// dispatches it until Signal() makes it ready — that's what "blocking" means here.
// Demo: a producer<->consumer ping-pong (each blocks on the other's signal,
// lock-step) plus a free-running task that proves the two really do block (it runs
// far more than the ping-pong cycles). No lost wakeups: Wait() checks tc_SigRecvd
// first, so a Signal that arrives before the Wait is not missed.

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

// ===== Forbid/Permit (compiler-barrier guarded, per the H6 lesson) ==========
static volatile sig_atomic_t forbid_cnt = 0;
static void Forbid(void) { forbid_cnt++; __asm__ volatile("" ::: "memory"); }
static void Permit(void) { __asm__ volatile("" ::: "memory"); if (forbid_cnt > 0) forbid_cnt--; }

// ===== Task + scheduler (H6 core) ===========================================
#define TS_RUN 2
#define TS_READY 3
#define TS_WAIT 4
struct Task {
    struct Node tc_Node; uint8_t tc_Flags, tc_State;
    uint32_t tc_SigRecvd, tc_SigWait;
    void *tc_SPReg, *tc_SPLower, *tc_SPUpper;
    _STRUCT_MCONTEXT64 tc_Context;
    void (*startpc)(struct Task *);
    long count, waits;
};
struct ExecBase { struct Task *ThisTask; struct List TaskReady, TaskWait; long DispCount; };
static struct ExecBase exb, *SysBase = &exb;
#define GET_THIS_TASK    (SysBase->ThisTask)
#define SET_THIS_TASK(t) (SysBase->ThisTask = (t))

#define NWORK 3
#define STK   (1 << 18)
#define TICK_LIMIT 400
static struct Task boot_task;
static struct Task *workers[NWORK];
static volatile sig_atomic_t sched_on = 0, inited = 0, ticks = 0;

static int core_Schedule(void) { return !IsListEmpty(&SysBase->TaskReady); }
static void core_Switch(void) {
    struct Task *t = GET_THIS_TASK;
    if (t->tc_State != TS_RUN) return;          // a task that has blocked (TS_WAIT) is NOT re-queued
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
static void cpu_Switch(_STRUCT_MCONTEXT64 *m) { struct Task *t = GET_THIS_TASK; t->tc_Context = *m; t->tc_SPReg = (void *)m->__ss.__sp; core_Switch(); }
static void cpu_Dispatch(_STRUCT_MCONTEXT64 *m) { struct Task *nt = core_Dispatch(); if (nt) *m = nt->tc_Context; }
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
            w->tc_Context.__ss.__x[0] = (uint64_t)w;
            w->tc_Context.__ss.__sp = (uint64_t)w->tc_SPReg;
            w->tc_Context.__ss.__fp = 0; w->tc_Context.__ss.__lr = 0;
        }
        inited = 1;
        return;
    }
    if (forbid_cnt > 0) return;
    if (ticks >= TICK_LIMIT) { sched_on = 0; SET_THIS_TASK(&boot_task); boot_task.tc_State = TS_RUN; *m = boot_task.tc_Context; return; }
    core_ExitInterrupt(m);
}

// ===== Wait() / Signal() — the blocking primitive ===========================
// Wait: block the current task until any bit of sigset is received.
static uint32_t Wait(uint32_t sigset) {
    struct Task *me = GET_THIS_TASK;
    Forbid();
    if (!(me->tc_SigRecvd & sigset)) {
        me->tc_SigWait = sigset;
        me->tc_State = TS_WAIT;
        Enqueue(&SysBase->TaskWait, &me->tc_Node);
        me->waits++;
        Permit();
        // Give up the CPU: once TS_WAIT, the next timer tick parks us (core_Switch
        // won't re-queue a non-RUN task) and dispatches someone else. We resume
        // here only after Signal() moves us back to TaskReady and we're dispatched
        // (the dispatcher sets TS_RUN). The volatile read defeats caching.
        while (*(volatile uint8_t *)&me->tc_State == TS_WAIT) { }
        Forbid();
    }
    uint32_t rcvd = me->tc_SigRecvd & sigset;
    me->tc_SigRecvd &= ~sigset;
    Permit();
    return rcvd;
}
// Signal: post bits to a task; if it was waiting on them, make it ready.
static void Signal(struct Task *task, uint32_t sigset) {
    Forbid();
    task->tc_SigRecvd |= sigset;
    if ((task->tc_SigRecvd & task->tc_SigWait) && task->tc_State == TS_WAIT) {
        Remove(&task->tc_Node);                 // out of TaskWait
        task->tc_State = TS_READY;
        Enqueue(&SysBase->TaskReady, &task->tc_Node);
    }
    Permit();
}

// ===== the demo: producer <-> consumer ping-pong + a free-runner ============
#define SIG_DATA (1u << 0)
#define SIG_ACK  (1u << 1)
static struct Task *t_prod, *t_cons;

static void producer(struct Task *self) {
    for (;;) { Signal(t_cons, SIG_DATA); self->count++; Wait(SIG_ACK); }
}
static void consumer(struct Task *self) {
    for (;;) { Wait(SIG_DATA); self->count++; Signal(t_prod, SIG_ACK); }
}
static void busy(struct Task *self) {
    for (;;) { self->count++; for (volatile long i = 0; i < 40000; i++) { } }
}

static struct Task *spawn(const char *name, int pri, void (*pc)(struct Task *)) {
    static struct Task pool[NWORK]; static int np;
    struct Task *t = &pool[np++];
    memset(t, 0, sizeof *t);
    t->tc_Node.ln_Name = (char *)name; t->tc_Node.ln_Pri = pri; t->startpc = pc;
    void *stk = mmap(NULL, STK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    t->tc_SPLower = stk; t->tc_SPUpper = (uint8_t *)stk + STK; t->tc_SPReg = (uint8_t *)stk + STK;
    t->tc_State = TS_READY;
    Enqueue(&SysBase->TaskReady, &t->tc_Node);
    return t;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H9a] hosted exec Wait/Signal: tasks block on TS_WAIT/TaskWait and wake on Signal\n");

    NewList(&SysBase->TaskReady);
    NewList(&SysBase->TaskWait);
    memset(&boot_task, 0, sizeof boot_task);
    boot_task.tc_Node.ln_Name = "boot"; boot_task.tc_Node.ln_Pri = -128; boot_task.tc_State = TS_RUN;
    SET_THIS_TASK(&boot_task);

    t_prod = spawn("producer", 0, producer);
    t_cons = spawn("consumer", 0, consumer);
    struct Task *t_busy = spawn("busy", 0, busy);
    workers[0] = t_prod; workers[1] = t_cons; workers[2] = t_busy;

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_alarm; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 5000; it.it_value = it.it_interval; // 200 Hz
    setitimer(ITIMER_REAL, &it, NULL);

    sched_on = 1;
    while (ticks < TICK_LIMIT) ;
    struct itimerval off; memset(&off, 0, sizeof off); setitimer(ITIMER_REAL, &off, NULL);

    long prod = t_prod->count, cons = t_cons->count, busyc = t_busy->count;
    int ok = 1;
    if (prod < 10)  { printf("[H9] FAIL: producer barely ran (%ld)\n", prod); ok = 0; }
    if (cons < 10)  { printf("[H9] FAIL: consumer barely ran (%ld)\n", cons); ok = 0; }
    if ((prod > cons ? prod - cons : cons - prod) > 2) { printf("[H9] FAIL: ping-pong not lock-step (p=%ld c=%ld)\n", prod, cons); ok = 0; }
    if (t_prod->waits < 5 || t_cons->waits < 5) { printf("[H9] FAIL: tasks didn't actually block (pw=%ld cw=%ld)\n", t_prod->waits, t_cons->waits); ok = 0; }
    // The ping-pong pair blocks each round, so the free-runner should out-run a
    // blocker (one of the pair is always parked -> busy gets a larger CPU share).
    if (busyc <= prod) { printf("[H9] FAIL: busy task didn't out-run a blocker (busy=%ld vs prod=%ld)\n", busyc, prod); ok = 0; }

    printf("[H9]   ping-pong rounds: producer=%ld consumer=%ld (lock-step); blocks: p=%ld c=%ld\n",
           prod, cons, t_prod->waits, t_cons->waits);
    printf("[H9]   free-running busy task ran %ld (>> %ld) -> blockers really yielded the CPU\n", busyc, prod);
    printf("[H9]   dispatched=%ld over %d ticks\n", SysBase->DispCount, (int)ticks);
    if (ok)
        printf("[H9] hosted exec Wait/Signal ok: tasks block on TS_WAIT and wake via Signal, no lost wakeups\n");
    else
        printf("[H9] FAIL: see checks above\n");
    return ok ? 0 : 1;
}
