// Hosted AArch64 AROS — Phase 2 H4: the AROS exec scheduler model, hosted.
//
// H2 proved macOS-hosted preemption with an ad-hoc round-robin. H4 reshapes it
// into AROS's REAL scheduler: a priority-ordered TaskReady list, struct Task with
// TS_* states, and the exact exec/kernel call graph
//
//     timer IRQ -> core_ExitInterrupt -> core_Schedule (BOOL: switch?)
//                                      -> cpu_Switch  (save regs; core_Switch:
//                                                      TS_RUN->TS_READY, Enqueue)
//                                      -> cpu_Dispatch (core_Dispatch: dequeue
//                                                       highest-pri ready; restore)
//
// grounded verbatim against arch/arm-native/kernel/{kernel_scheduler.c,kernel_cpu.c}
// and compiler/include/exec/tasks.h in the upstream tree. The hosted arch layer
// (cpu_Switch/cpu_Dispatch) saves/restores through the SIGALRM mcontext — the H2
// mechanism — but now ThisTask/TaskReady and the priority logic are AROS's.
//
// What this proves beyond H2: priority dispatch + round-robin among equals +
// strict-priority starvation, all from the real scheduler shapes. Two pri-1 tasks
// alternate fairly; two pri-0 tasks never run while a pri-1 is ready.
//
// Faithfulness notes: this is a focused spike, so it carries a minimal exec list
// (real Enqueue/GetHead/Remove semantics) and a struct Task SUBSET — the fields
// the scheduler touches. `startpc` is a hosted-only helper (real AROS materialises
// the first frame in AddTask via cpu_Init); we lazily build each task's context
// from a live mcontext template on the first tick, the H2-proven way.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

// ---- minimal exec list (real AROS Node/List semantics) ---------------------
struct Node {
    struct Node *ln_Succ, *ln_Pred;
    uint8_t      ln_Type;
    int8_t       ln_Pri;
    char        *ln_Name;
};
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; uint8_t lh_Type; };

static void NewList(struct List *l) {
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}
static int IsListEmpty(struct List *l) { return l->lh_TailPred == (struct Node *)l; }
static struct Node *GetHead(struct List *l) {
    return IsListEmpty(l) ? NULL : l->lh_Head;
}
static void Remove(struct Node *n) {
    n->ln_Pred->ln_Succ = n->ln_Succ;
    n->ln_Succ->ln_Pred = n->ln_Pred;
}
// AROS Enqueue: insert AFTER the last node of equal-or-higher priority (FIFO
// among equals) — i.e. before the first node of strictly lower priority.
static void Enqueue(struct List *l, struct Node *n) {
    struct Node *p;
    for (p = l->lh_Head; p->ln_Succ; p = p->ln_Succ)
        if (p->ln_Pri < n->ln_Pri) break;
    n->ln_Pred = p->ln_Pred;
    n->ln_Succ = p;
    p->ln_Pred->ln_Succ = n;
    p->ln_Pred = n;
}

// ---- struct Task (subset the scheduler touches) + TS_* states --------------
#define TS_INVALID 0
#define TS_ADDED   1
#define TS_RUN     2
#define TS_READY   3
#define TS_WAIT    4

struct Task {
    struct Node       tc_Node;
    uint8_t           tc_Flags;
    uint8_t           tc_State;
    void             *tc_SPReg;     // current SP (tracked on save, like AROS)
    void             *tc_SPLower;   // stack bounds (mmap'd region)
    void             *tc_SPUpper;
    _STRUCT_MCONTEXT64 tc_Context;  // hosted analog of et_RegFrame
    void            (*startpc)(void);   // hosted-only: first-frame entry
    long              ran;              // demo: how many quanta this task got
};

// ---- ExecBase subset --------------------------------------------------------
struct ExecBase {
    struct Task *ThisTask;
    struct List  TaskReady;
    long         DispCount;
    long         IdleCount;
};
static struct ExecBase exb;
static struct ExecBase *SysBase = &exb;
#define GET_THIS_TASK      (SysBase->ThisTask)
#define SET_THIS_TASK(t)   (SysBase->ThisTask = (t))

// ---------------------------------------------------------------------------
#define NWORK 4
#define STK   (1 << 18)                 // 256 KiB, page-multiple
static struct Task boot_task;           // the macOS main thread, as a Task
static struct Task work[NWORK];
#define TICK_LIMIT 300
static volatile sig_atomic_t sched_on = 0;
static volatile sig_atomic_t inited    = 0;
static volatile sig_atomic_t ticks     = 0;

// The four worker bodies: bump a counter, burn a fixed slice. No printf (stdio
// lock would deadlock if a preemption lands mid-print).
static void body(struct Task *self) {
    for (;;) { self->ran++; for (volatile long i = 0; i < 300000; i++) {} }
}
static void w0(void){ body(&work[0]); }
static void w1(void){ body(&work[1]); }
static void w2(void){ body(&work[2]); }
static void w3(void){ body(&work[3]); }

// AddTask: register a task READY at a priority, with an mmap'd stack.
static void AddTask(struct Task *t, const char *name, int pri, void (*pc)(void)) {
    memset(t, 0, sizeof *t);
    t->tc_Node.ln_Name = (char *)name;
    t->tc_Node.ln_Pri  = pri;
    t->startpc = pc;
    void *stk = mmap(NULL, STK, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    t->tc_SPLower = stk;
    t->tc_SPUpper = (char *)stk + STK;
    t->tc_SPReg   = (char *)stk + STK;     // 16-byte aligned (page top)
    t->tc_State   = TS_ADDED;
    Enqueue(&SysBase->TaskReady, &t->tc_Node);
    t->tc_State   = TS_READY;
}

// ---- the arch + core scheduler (faithful contracts) ------------------------

// core_Schedule: should the running task yield the CPU? We treat every tick as a
// quantum boundary, so: reschedule whenever another task is ready. (AROS also
// keeps a non-expired quantum running; mirror is in the comment.)
static int core_Schedule(void) {
    if (IsListEmpty(&SysBase->TaskReady))
        return 0;                          // nobody else ready: let it run
    return 1;                              // quantum expired -> reschedule
}

// core_Switch: running task TS_RUN -> TS_READY, back into the priority list.
// The boot anchor (the macOS main thread) is saved but never re-enters the ready
// list — it's our shutdown anchor, restored explicitly by the timer at the end.
static void core_Switch(void) {
    struct Task *t = GET_THIS_TASK;
    if (t->tc_State != TS_RUN)
        return;
    if (t != &boot_task &&
        (t->tc_SPReg <= t->tc_SPLower || t->tc_SPReg > t->tc_SPUpper)) {
        // AROS calls Alert(AN_StackProbe) here; for the spike, flag and park.
        t->tc_State = TS_WAIT;
        return;
    }
    t->tc_State = TS_READY;
    if (t != &boot_task)
        Enqueue(&SysBase->TaskReady, &t->tc_Node);
}

// core_Dispatch: pick + dequeue the highest-priority ready task, mark TS_RUN.
static struct Task *core_Dispatch(void) {
    struct Node *n = GetHead(&SysBase->TaskReady);
    if (!n) { SysBase->IdleCount++; return NULL; }
    Remove(n);
    struct Task *nt = (struct Task *)n;
    SysBase->DispCount++;
    SET_THIS_TASK(nt);
    nt->tc_State = TS_RUN;
    return nt;
}

// cpu_Switch: save the preempted task's registers (hosted: the signal mcontext)
// into its context frame, track SP, then core_Switch().
static void cpu_Switch(_STRUCT_MCONTEXT64 *m) {
    struct Task *t = GET_THIS_TASK;
    t->tc_Context = *m;
    t->tc_SPReg   = (void *)m->__ss.__sp;
    core_Switch();
}

// cpu_Dispatch: get the next task and restore its registers into the mcontext,
// so the handler returns into it. (No WFI idle loop hosted — main always exists.)
static void cpu_Dispatch(_STRUCT_MCONTEXT64 *m) {
    struct Task *nt = core_Dispatch();
    if (!nt) return;                       // nothing ready: stay put
    *m = nt->tc_Context;
}

// ---- the hosted "timer IRQ": SIGALRM -> core_ExitInterrupt -----------------
static void core_ExitInterrupt(_STRUCT_MCONTEXT64 *m) {
    if (core_Schedule()) {
        cpu_Switch(m);
        cpu_Dispatch(m);
    }
}

static void on_alarm(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    _STRUCT_MCONTEXT64 *m = uc->uc_mcontext;
    ticks++;

    if (!sched_on)
        return;

    if (!inited) {
        // First armed tick: capture a live mcontext template (= boot task) and
        // materialise every TS_ADDED/READY worker's first frame from it.
        boot_task.tc_Context = *m;
        for (int i = 0; i < NWORK; i++) {
            work[i].tc_Context = *m;                    // valid template
            memset(work[i].tc_Context.__ss.__x, 0, sizeof work[i].tc_Context.__ss.__x);
            work[i].tc_Context.__ss.__pc = (uint64_t)work[i].startpc;
            work[i].tc_Context.__ss.__sp = (uint64_t)work[i].tc_SPReg;
            work[i].tc_Context.__ss.__fp = 0;
            work[i].tc_Context.__ss.__lr = 0;
        }
        inited = 1;
        return;                                          // stay in boot task
    }

    if (ticks >= TICK_LIMIT) {
        // Shutdown: stop switching and restore the boot anchor so the macOS main
        // thread resumes (its parked busy-loop) and runs the cleanup/print path.
        sched_on = 0;
        SET_THIS_TASK(&boot_task);
        boot_task.tc_State = TS_RUN;
        *m = boot_task.tc_Context;
        return;
    }

    core_ExitInterrupt(m);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H4a] hosted AROS scheduler: priority TaskReady + core_Schedule/cpu_Switch/cpu_Dispatch\n");

    NewList(&SysBase->TaskReady);
    // boot_task = the macOS main thread; lowest priority so it never wins while
    // workers are ready (it's our shutdown anchor, not in the ready list).
    memset(&boot_task, 0, sizeof boot_task);
    boot_task.tc_Node.ln_Name = "boot";
    boot_task.tc_Node.ln_Pri  = -128;
    boot_task.tc_State        = TS_RUN;
    SET_THIS_TASK(&boot_task);

    // Two tasks at pri 1 (should fairly round-robin), two at pri 0 (should starve).
    AddTask(&work[0], "A.pri1", 1, w0);
    AddTask(&work[1], "B.pri1", 1, w1);
    AddTask(&work[2], "C.pri0", 0, w2);
    AddTask(&work[3], "D.pri0", 0, w3);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_alarm;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it;
    it.it_interval.tv_sec = 0;  it.it_interval.tv_usec = 10000;   // 100 Hz
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);

    sched_on = 1;
    // This loop is the boot anchor's body. After the first scheduling tick we're
    // parked (switched out); the timer restores us at TICK_LIMIT, we resume here
    // with ticks >= TICK_LIMIT, fall through, and clean up.
    while (ticks < TICK_LIMIT)
        ;
    struct itimerval off; memset(&off, 0, sizeof off);
    setitimer(ITIMER_REAL, &off, NULL);

    printf("[H4]   dispatched=%ld over %d ticks\n", SysBase->DispCount, (int)ticks);
    printf("[H4]   pri1: A=%ld B=%ld   pri0: C=%ld D=%ld\n",
           work[0].ran, work[1].ran, work[2].ran, work[3].ran);

    long a = work[0].ran, b = work[1].ran, c = work[2].ran, d = work[3].ran;
    int pri1_ran  = a > 50 && b > 50;          // both high-pri tasks got the CPU
    int pri1_fair = (a > b ? a - b : b - a) <= (a + b) / 4 + 2;  // fair-ish split
    int pri0_starved = c == 0 && d == 0;       // strict priority starves pri-0

    if (pri1_ran && pri1_fair && pri0_starved)
        printf("[H4] hosted AROS scheduler ok: pri-1 round-robins fairly, pri-0 starved (strict priority)\n");
    else
        printf("[H4] FAIL: ran=%d fair=%d starved=%d\n", pri1_ran, pri1_fair, pri0_starved);
    return (pri1_ran && pri1_fair && pri0_starved) ? 0 : 1;
}
