// Hosted AArch64 AROS — Phase 2 H10: message ports — exec IPC, the device-I/O shape.
//
// Almost everything in AROS talks via messages: a task PutMsg()s a Message to a
// MsgPort and the owner WaitPort()s for it; device I/O is exactly this (send an
// IORequest, wait the reply). H10 layers the real port mechanism on H9's
// Wait/Signal, grounded against rom/exec/{putmsg,getmsg,waitport}.c:
//
//   PutMsg(port,msg):  AddTail(port->mp_MsgList, msg); Signal(mp_SigTask, 1<<mp_SigBit)
//   WaitPort(port):    while empty: Wait(1<<mp_SigBit); return head (peek)
//   GetMsg(port):      Disable; RemHead(mp_MsgList); Enable
//   ReplyMsg(msg):     PutMsg(msg->mn_ReplyPort, msg)
//
// Demo: a server task WaitPort()s its request port, squares each request, and
// ReplyMsg()s; a client fires request->reply round-trips and verifies result ==
// n*n. Both BLOCK on their ports (no busy-wait), under the preemptive timer. This
// is the canonical client/server I/O loop a hosted AROS uses to reach host
// resources — proven hosted on macOS.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

// ===== exec lists (incl. AddTail/RemHead for message queues) ================
struct Node { struct Node *ln_Succ, *ln_Pred; uint8_t ln_Type; int8_t ln_Pri; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; uint8_t lh_Type; };
static void NewList(struct List *l) { l->lh_Head = (struct Node *)&l->lh_Tail; l->lh_Tail = NULL; l->lh_TailPred = (struct Node *)&l->lh_Head; }
static int IsListEmpty(struct List *l) { return l->lh_TailPred == (struct Node *)l; }
static struct Node *GetHead(struct List *l) { return IsListEmpty(l) ? NULL : l->lh_Head; }
static void Remove(struct Node *n) { n->ln_Pred->ln_Succ = n->ln_Succ; n->ln_Succ->ln_Pred = n->ln_Pred; }
static void AddTail(struct List *l, struct Node *n) { n->ln_Succ = (struct Node *)&l->lh_Tail; n->ln_Pred = l->lh_TailPred; l->lh_TailPred->ln_Succ = n; l->lh_TailPred = n; }
static struct Node *RemHead(struct List *l) { struct Node *n = GetHead(l); if (n) Remove(n); return n; }
static void Enqueue(struct List *l, struct Node *n) {
    struct Node *p;
    for (p = l->lh_Head; p->ln_Succ; p = p->ln_Succ) if (p->ln_Pri < n->ln_Pri) break;
    n->ln_Pred = p->ln_Pred; n->ln_Succ = p; p->ln_Pred->ln_Succ = n; p->ln_Pred = n;
}

// ===== Forbid/Permit (compiler-barrier guarded) =============================
static volatile sig_atomic_t forbid_cnt = 0;
static void Forbid(void) { forbid_cnt++; __asm__ volatile("" ::: "memory"); }
static void Permit(void) { __asm__ volatile("" ::: "memory"); if (forbid_cnt > 0) forbid_cnt--; }

// ===== Task + scheduler + Wait/Signal (H6/H9 core) ==========================
#define TS_RUN 2
#define TS_READY 3
#define TS_WAIT 4
struct Task {
    struct Node tc_Node; uint8_t tc_Flags, tc_State;
    uint32_t tc_SigRecvd, tc_SigWait;
    void *tc_SPReg, *tc_SPLower, *tc_SPUpper;
    _STRUCT_MCONTEXT64 tc_Context;
    void (*startpc)(struct Task *);
};
struct ExecBase { struct Task *ThisTask; struct List TaskReady, TaskWait; long DispCount; };
static struct ExecBase exb, *SysBase = &exb;
#define GET_THIS_TASK    (SysBase->ThisTask)
#define SET_THIS_TASK(t) (SysBase->ThisTask = (t))

#define NWORK 3
#define STK   (1 << 18)
#define TICK_LIMIT 500
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
    Remove(n); struct Task *nt = (struct Task *)n;
    SysBase->DispCount++; SET_THIS_TASK(nt); nt->tc_State = TS_RUN; return nt;
}
static void cpu_Switch(_STRUCT_MCONTEXT64 *m) { struct Task *t = GET_THIS_TASK; t->tc_Context = *m; t->tc_SPReg = (void *)m->__ss.__sp; core_Switch(); }
static void cpu_Dispatch(_STRUCT_MCONTEXT64 *m) { struct Task *nt = core_Dispatch(); if (nt) *m = nt->tc_Context; }
static void core_ExitInterrupt(_STRUCT_MCONTEXT64 *m) { if (core_Schedule()) { cpu_Switch(m); cpu_Dispatch(m); } }

static void on_alarm(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv; _STRUCT_MCONTEXT64 *m = uc->uc_mcontext;
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
    core_ExitInterrupt(m);
}

static uint32_t Wait(uint32_t sigset) {
    struct Task *me = GET_THIS_TASK;
    Forbid();
    if (!(me->tc_SigRecvd & sigset)) {
        me->tc_SigWait = sigset; me->tc_State = TS_WAIT; Enqueue(&SysBase->TaskWait, &me->tc_Node);
        Permit();
        while (*(volatile uint8_t *)&me->tc_State == TS_WAIT) { }
        Forbid();
    }
    uint32_t rcvd = me->tc_SigRecvd & sigset; me->tc_SigRecvd &= ~sigset;
    Permit(); return rcvd;
}
static void Signal(struct Task *task, uint32_t sigset) {
    Forbid();
    task->tc_SigRecvd |= sigset;
    if ((task->tc_SigRecvd & task->tc_SigWait) && task->tc_State == TS_WAIT) {
        Remove(&task->tc_Node); task->tc_State = TS_READY; Enqueue(&SysBase->TaskReady, &task->tc_Node);
    }
    Permit();
}

// ===== message ports (exec/ports.h) =========================================
#define PA_SIGNAL 0
struct MsgPort {
    struct Node  mp_Node;
    uint8_t      mp_Flags;
    uint8_t      mp_SigBit;
    struct Task *mp_SigTask;
    struct List  mp_MsgList;
};
struct Message {
    struct Node     mn_Node;
    struct MsgPort *mn_ReplyPort;
    uint16_t        mn_Length;
};
static void NewPort(struct MsgPort *p, struct Task *sigtask, uint8_t sigbit) {
    memset(p, 0, sizeof *p);
    p->mp_Flags = PA_SIGNAL; p->mp_SigTask = sigtask; p->mp_SigBit = sigbit;
    NewList(&p->mp_MsgList);
}
static void PutMsg(struct MsgPort *port, struct Message *msg) {
    Forbid();
    AddTail(&port->mp_MsgList, &msg->mn_Node);
    Permit();
    if (port->mp_SigTask && (port->mp_Flags & 0x3) == PA_SIGNAL)
        Signal(port->mp_SigTask, 1u << port->mp_SigBit);
}
static struct Message *GetMsg(struct MsgPort *port) {
    Forbid();
    struct Message *m = (struct Message *)RemHead(&port->mp_MsgList);
    Permit();
    return m;
}
static struct Message *WaitPort(struct MsgPort *port) {
    while (IsListEmpty(&port->mp_MsgList))
        Wait(1u << port->mp_SigBit);
    return (struct Message *)GetHead(&port->mp_MsgList);
}
static void ReplyMsg(struct Message *msg) { PutMsg(msg->mn_ReplyPort, msg); }

// ===== the demo: client <-> server request/reply ============================
#define SIGB_REQ   8
#define SIGB_REPLY 9
struct ReqMsg { struct Message msg; long value; long result; };
static struct MsgPort req_port, reply_port;
static struct Task *t_server, *t_client;
static volatile long g_rounds, g_errors, g_served;

static void server(struct Task *self) {
    (void)self;
    for (;;) {
        WaitPort(&req_port);
        struct Message *m;
        while ((m = GetMsg(&req_port))) {                 // drain the queue
            struct ReqMsg *r = (struct ReqMsg *)m;
            r->result = r->value * r->value;              // "device" work
            g_served++;
            ReplyMsg(m);
        }
    }
}
static void client(struct Task *self) {
    (void)self;
    static struct ReqMsg req;                             // one in flight at a time
    req.msg.mn_ReplyPort = &reply_port;
    long n = 0;
    for (;;) {
        n++;
        req.value = n; req.result = -1;
        PutMsg(&req_port, &req.msg);                      // send request
        struct Message *m = WaitPort(&reply_port);        // block for the reply
        GetMsg(&reply_port);                              // dequeue it
        struct ReqMsg *r = (struct ReqMsg *)m;
        if (r->result != r->value * r->value) g_errors++; // verify correctness
        g_rounds++;
    }
}
static void idle(struct Task *self) { for (;;) { (void)self; for (volatile long i = 0; i < 40000; i++) { } } }

static struct Task *spawn(const char *name, void (*pc)(struct Task *)) {
    static struct Task pool[NWORK]; static int np;
    struct Task *t = &pool[np++];
    memset(t, 0, sizeof *t);
    t->tc_Node.ln_Name = (char *)name; t->tc_Node.ln_Pri = 0; t->startpc = pc;
    void *stk = mmap(NULL, STK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    t->tc_SPLower = stk; t->tc_SPUpper = (uint8_t *)stk + STK; t->tc_SPReg = (uint8_t *)stk + STK;
    t->tc_State = TS_READY; Enqueue(&SysBase->TaskReady, &t->tc_Node);
    return t;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H10a] hosted exec message ports: PutMsg/WaitPort/GetMsg/ReplyMsg on Wait/Signal\n");

    NewList(&SysBase->TaskReady); NewList(&SysBase->TaskWait);
    memset(&boot_task, 0, sizeof boot_task);
    boot_task.tc_Node.ln_Name = "boot"; boot_task.tc_Node.ln_Pri = -128; boot_task.tc_State = TS_RUN;
    SET_THIS_TASK(&boot_task);

    t_server = spawn("server", server);
    t_client = spawn("client", client);
    struct Task *t_idle = spawn("idle", idle);
    workers[0] = t_server; workers[1] = t_client; workers[2] = t_idle;
    NewPort(&req_port,   t_server, SIGB_REQ);
    NewPort(&reply_port, t_client, SIGB_REPLY);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_alarm; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 5000; it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);

    sched_on = 1;
    while (ticks < TICK_LIMIT) ;
    struct itimerval off; memset(&off, 0, sizeof off); setitimer(ITIMER_REAL, &off, NULL);

    long rounds = g_rounds, errors = g_errors, served = g_served;
    int ok = 1;
    if (rounds < 20) { printf("[H10] FAIL: too few round-trips (%ld)\n", rounds); ok = 0; }
    if (errors != 0) { printf("[H10] FAIL: %ld wrong replies\n", errors); ok = 0; }
    if (served < rounds - 1 || served > rounds + 1) { printf("[H10] FAIL: served=%ld vs rounds=%ld (msg loss?)\n", served, rounds); ok = 0; }

    printf("[H10]   request/reply round-trips=%ld, server processed=%ld, wrong replies=%ld\n", rounds, served, errors);
    printf("[H10]   dispatched=%ld over %d ticks (client+server block on their ports)\n", SysBase->DispCount, (int)ticks);
    if (ok)
        printf("[H10] hosted exec message ports ok: PutMsg/WaitPort/GetMsg/ReplyMsg round-trips correct, no loss\n");
    else
        printf("[H10] FAIL: see checks above\n");
    return ok ? 0 : 1;
}
