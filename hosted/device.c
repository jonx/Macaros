// Hosted AArch64 AROS — Phase 2 H11: a device backed by a real macOS file.
//
// THE Phase-2 thesis, end to end: "macOS owns the drivers, AROS reaches them via
// standard exec I/O." A client builds an IORequest and DoIO()s it; the request
// travels the exec I/O path (BeginIO -> PutMsg -> a device task -> ReplyMsg ->
// WaitIO) and the device performs the actual work on a REAL macOS resource — here
// pread/pwrite on a real file. So the bytes genuinely round-trip:
//
//   AROS client  --IORequest/DoIO-->  message port  -->  device task
//                                                         --> pwrite()/pread() on a real file
//   AROS client  <--reply/WaitIO----  message port  <--  ReplyMsg
//
// Grounded against exec/io.h (IOStdReq layout, CMD_READ=2/CMD_WRITE=3) and
// rom/exec/doio.c (DoIO = BeginIO then, if not quick, WaitIO). Built on the H10
// message ports (and thus H9 Wait/Signal, H6 scheduler). The host syscalls run on
// a switched task stack under preemption — the H1 property, now at the device
// layer. Verified two ways: the client checks read==written, and main then reads
// the file INDEPENDENTLY via the host to confirm the data physically landed.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
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
static void AddTail(struct List *l, struct Node *n) { n->ln_Succ = (struct Node *)&l->lh_Tail; n->ln_Pred = l->lh_TailPred; l->lh_TailPred->ln_Succ = n; l->lh_TailPred = n; }
static struct Node *RemHead(struct List *l) { struct Node *n = GetHead(l); if (n) Remove(n); return n; }
static void Enqueue(struct List *l, struct Node *n) {
    struct Node *p;
    for (p = l->lh_Head; p->ln_Succ; p = p->ln_Succ) if (p->ln_Pri < n->ln_Pri) break;
    n->ln_Pred = p->ln_Pred; n->ln_Succ = p; p->ln_Pred->ln_Succ = n; p->ln_Pred = n;
}

// ===== Forbid/Permit ========================================================
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

// ===== message ports (H10) ==================================================
struct MsgPort { struct Node mp_Node; uint8_t mp_Flags; uint8_t mp_SigBit; struct Task *mp_SigTask; struct List mp_MsgList; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; uint16_t mn_Length; };
static void NewPort(struct MsgPort *p, struct Task *sigtask, uint8_t sigbit) { memset(p, 0, sizeof *p); p->mp_SigTask = sigtask; p->mp_SigBit = sigbit; NewList(&p->mp_MsgList); }
static void PutMsg(struct MsgPort *port, struct Message *msg) { Forbid(); AddTail(&port->mp_MsgList, &msg->mn_Node); Permit(); if (port->mp_SigTask) Signal(port->mp_SigTask, 1u << port->mp_SigBit); }
static struct Message *GetMsg(struct MsgPort *port) { Forbid(); struct Message *m = (struct Message *)RemHead(&port->mp_MsgList); Permit(); return m; }
static struct Message *WaitPort(struct MsgPort *port) { while (IsListEmpty(&port->mp_MsgList)) Wait(1u << port->mp_SigBit); return (struct Message *)GetHead(&port->mp_MsgList); }
static void ReplyMsg(struct Message *msg) { PutMsg(msg->mn_ReplyPort, msg); }

// ===== device / IORequest layer (exec/io.h) =================================
#define CMD_READ  2
#define CMD_WRITE 3
struct IOStdReq {
    struct Message  io_Message;          // MUST be first: (Message*)io == &io->io_Message
    void           *io_Device;
    void           *io_Unit;
    uint16_t        io_Command;
    uint8_t         io_Flags;
    int8_t          io_Error;
    uint32_t        io_Actual;
    uint32_t        io_Length;
    void           *io_Data;
    uint32_t        io_Offset;
};

#define SIGB_DEV   8
#define SIGB_CLI   9
static struct MsgPort dev_port, cli_port;
static struct Task *t_device, *t_client;
static int dev_fd = -1;                  // the real macOS file the device drives

// The device task: the only code that touches the host file. WaitPort for an
// IORequest, perform the real syscall, ReplyMsg. This is "macOS owns the driver".
static void device_task(struct Task *self) {
    (void)self;
    for (;;) {
        WaitPort(&dev_port);
        struct Message *m;
        while ((m = GetMsg(&dev_port))) {
            struct IOStdReq *io = (struct IOStdReq *)m;     // io_Message is first
            ssize_t n = -1;
            if (io->io_Command == CMD_WRITE)
                n = pwrite(dev_fd, io->io_Data, io->io_Length, io->io_Offset);
            else if (io->io_Command == CMD_READ)
                n = pread(dev_fd, io->io_Data, io->io_Length, io->io_Offset);
            io->io_Actual = (n < 0) ? 0 : (uint32_t)n;
            io->io_Error  = (n == (ssize_t)io->io_Length) ? 0 : -1;
            ReplyMsg(&io->io_Message);
        }
    }
}

// DoIO: the exec I/O call. BeginIO (send to the device port) then WaitIO (block
// for the reply on our port). Grounded shape from rom/exec/doio.c.
static int8_t DoIO(struct IOStdReq *io) {
    io->io_Message.mn_ReplyPort = &cli_port;
    io->io_Message.mn_Node.ln_Type = 0;
    PutMsg(&dev_port, &io->io_Message);   // BeginIO (async device)
    WaitPort(&cli_port);                   // WaitIO
    GetMsg(&cli_port);
    return io->io_Error;
}

static volatile long g_rounds, g_errors;
static volatile unsigned char g_last_pat;

#define BUFSZ 128
static void client_task(struct Task *self) {
    (void)self;
    struct IOStdReq io; memset(&io, 0, sizeof io);
    unsigned char wbuf[BUFSZ], rbuf[BUFSZ];
    long round = 0;
    for (;;) {
        round++;
        unsigned char pat = (unsigned char)(round & 0xFF);
        memset(wbuf, pat, sizeof wbuf);
        // CMD_WRITE the pattern to the real file via the exec I/O path
        io.io_Command = CMD_WRITE; io.io_Data = wbuf; io.io_Length = BUFSZ; io.io_Offset = 0;
        if (DoIO(&io)) { g_errors++; continue; }
        // CMD_READ it back into a cleared buffer and verify
        memset(rbuf, 0, sizeof rbuf);
        io.io_Command = CMD_READ; io.io_Data = rbuf; io.io_Length = BUFSZ; io.io_Offset = 0;
        if (DoIO(&io)) { g_errors++; continue; }
        if (memcmp(wbuf, rbuf, BUFSZ) != 0) g_errors++;
        g_last_pat = pat;
        g_rounds++;
    }
}
static void idle_task(struct Task *self) { for (;;) { (void)self; for (volatile long i = 0; i < 40000; i++) { } } }

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
    printf("[H11a] hosted device: AROS exec I/O (DoIO/IORequest) -> a real macOS file\n");

    const char *path = "run/h11.dat";
    dev_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (dev_fd < 0) { printf("[H11] FAIL: open %s\n", path); return 1; }

    NewList(&SysBase->TaskReady); NewList(&SysBase->TaskWait);
    memset(&boot_task, 0, sizeof boot_task);
    boot_task.tc_Node.ln_Name = "boot"; boot_task.tc_Node.ln_Pri = -128; boot_task.tc_State = TS_RUN;
    SET_THIS_TASK(&boot_task);

    t_device = spawn("device", device_task);
    t_client = spawn("client", client_task);
    struct Task *t_idle = spawn("idle", idle_task);
    workers[0] = t_device; workers[1] = t_client; workers[2] = t_idle;
    NewPort(&dev_port, t_device, SIGB_DEV);
    NewPort(&cli_port, t_client, SIGB_CLI);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_alarm; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 5000; it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);

    sched_on = 1;
    while (ticks < TICK_LIMIT) ;
    struct itimerval off; memset(&off, 0, sizeof off); setitimer(ITIMER_REAL, &off, NULL);

    long rounds = g_rounds, errors = g_errors;
    unsigned char pat = g_last_pat;
    int ok = 1;
    if (rounds < 20) { printf("[H11] FAIL: too few I/O round-trips (%ld)\n", rounds); ok = 0; }
    if (errors != 0) { printf("[H11] FAIL: %ld I/O errors/mismatches\n", errors); ok = 0; }

    // Independent proof the bytes really landed in a real macOS file: read it here.
    unsigned char check[BUFSZ]; memset(check, 0, sizeof check);
    ssize_t rn = pread(dev_fd, check, BUFSZ, 0);
    int file_ok = (rn == BUFSZ);
    for (int i = 0; i < BUFSZ && file_ok; i++) if (check[i] != pat) file_ok = 0;
    if (!file_ok) { printf("[H11] FAIL: host re-read of %s didn't match last write (pat=%u)\n", path, pat); ok = 0; }
    close(dev_fd);

    printf("[H11]   DoIO round-trips=%ld (CMD_WRITE+CMD_READ), errors=%ld\n", rounds, errors);
    printf("[H11]   independent host read of %s: %d bytes, all == 0x%02X -> %s\n",
           path, (int)rn, pat, file_ok ? "MATCH" : "MISMATCH");
    printf("[H11]   dispatched=%ld over %d ticks (client+device block on their ports)\n", SysBase->DispCount, (int)ticks);
    if (ok)
        printf("[H11] hosted device ok: AROS exec I/O drove a real macOS file end-to-end (macOS owns the driver)\n");
    else
        printf("[H11] FAIL: see checks above\n");
    return ok ? 0 : 1;
}
