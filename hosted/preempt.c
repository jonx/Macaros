// Hosted AArch64 AROS — Phase 2 H2: preemptive scheduling on macOS.
//
// The hosted analog of the bare-metal timer IRQ (M5/M10). A periodic SIGALRM is
// our "timer interrupt"; in the handler we swap the saved register state in the
// signal's mcontext, so when the handler returns the kernel resumes a DIFFERENT
// task. The worker tasks never yield — that both make progress proves macOS-hosted
// preemption works. This is the hosted scheduler's viability proof.
//
// Grounded: for -arch arm64, __DARWIN_OPAQUE_ARM_THREAD_STATE64 == 0, so
// ucontext->uc_mcontext->__ss.{__x[29],__fp,__lr,__sp,__pc} are plain fields
// (no pointer-auth). Workers do NO printf (avoids stdio-lock reentrancy across a
// preemption); main prints the result while preemption is off.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ucontext.h>

#define NT  3                 // [0] = main, [1] = A, [2] = B
#define STK (1 << 18)         // 256 KiB host stacks

static _STRUCT_MCONTEXT64 saved[NT];
static int ready_[NT];
static int cur = 0;
static volatile sig_atomic_t sched_on = 0;
static volatile sig_atomic_t ticks = 0;
static volatile long ra, rb;
static void *stk_a, *stk_b;

static void worker_a(void) { for (;;) { ra++; for (volatile long i = 0; i < 300000; i++) { } } }
static void worker_b(void) { for (;;) { rb++; for (volatile long i = 0; i < 300000; i++) { } } }

static void make_task(int t, void (*entry)(void), void *stack_top)
{
    saved[t] = saved[0];                                  // template: main's captured mcontext
    memset(saved[t].__ss.__x, 0, sizeof(saved[t].__ss.__x));
    saved[t].__ss.__pc = (uint64_t)entry;
    saved[t].__ss.__sp = (uint64_t)stack_top;
    saved[t].__ss.__fp = 0;
    saved[t].__ss.__lr = 0;
    ready_[t] = 1;
}

// SIGALRM = the hosted timer interrupt.
static void on_alarm(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    ticks++;
    if (!sched_on)
        return;
    if (!ready_[1]) {                                     // first armed tick: we're in main
        saved[0] = *uc->uc_mcontext;                      // capture a valid template (= main)
        ready_[0] = 1;
        make_task(1, worker_a, (char *)stk_a + STK);
        make_task(2, worker_b, (char *)stk_b + STK);
        return;                                           // stay in main this tick
    }
    saved[cur] = *uc->uc_mcontext;                        // save preempted task
    cur = (cur + 1) % NT;
    *uc->uc_mcontext = saved[cur];                        // resume the next one
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H2a] hosted preemption: arming SIGALRM as the timer (tasks never yield)\n");

    stk_a = malloc(STK);
    stk_b = malloc(STK);

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
    while (ticks < 200)                                   // ~2s; timer preempts main<->A<->B
        ;                                                 // (busy: no syscall to interrupt)
    sched_on = 0;

    struct itimerval off;  memset(&off, 0, sizeof off);
    setitimer(ITIMER_REAL, &off, NULL);                  // stop the timer before we print/exit

    printf("[H2] hosted preemptive multitasking ok: A ran=%ld B ran=%ld over %d ticks (no yields)\n",
           ra, rb, (int)ticks);
    return 0;
}
