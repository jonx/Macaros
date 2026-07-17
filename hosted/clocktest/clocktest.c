/*
 * ClockTest -- does AROS's monotonic clock keep advancing under a spinning
 * low-priority thread, and does a WAITing high-priority task get scheduled
 * over a RUNning low-priority one?
 *
 * This isolates the Feraille freeze. A live frozen instance showed the guest
 * monotonic clock (CLOCK_MONOTONIC / GetUpTime) FROZEN for >2 minutes of wall
 * time while one pri -1 worker thread was pinned at 100% CPU inside a
 * timed-wait. The timed-wait was behaving correctly -- it measures elapsed
 * against that clock, and a frozen clock means "no time has passed", so it
 * never times out, never blocks, and holds the single guest CPU forever:
 *
 *   clock frozen -> timed waits never expire -> thread never blocks ->
 *   CPU pinned -> clock stays frozen -> ...
 *
 * TimerTest proved the primitives work with a HEALTHY clock. This asks the
 * question TimerTest didn't: what happens to the clock UNDER LOAD?
 *
 * Ground truth is the HOST: the hosted timer.device is driven by the host's
 * SIGALRM/real clock, so a real timer.device request completing tells us wall
 * time passed even if the guest's software time counter is stuck. Each test
 * therefore cross-checks the guest clock against an actual timer.device wait.
 *
 * Tests:
 *   A  baseline: does CLOCK_MONOTONIC advance across timer.device waits when
 *      nothing else runs? (sanity -- must PASS)
 *   B  spawn a pri -1 thread spinning in a tight clock_gettime() loop (never
 *      blocking). Then, from the pri 0 main thread, wait 100ms on
 *      timer.device 5x and check CLOCK_MONOTONIC advanced each time. FROZEN
 *      clock or a main thread that never runs => reproduced.
 *   C  same, but the spinner does pthread_cond_timedwait(1ms) in a loop -- the
 *      exact pattern the app's idle worker uses.
 *   D  preemption: does a RUNNING pri -1 spinner get preempted so a pri 0
 *      task that becomes READY actually runs within a bounded time?
 *
 * Usage: ClockTest
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <exec/types.h>
#include <exec/tasks.h>
#include <devices/timer.h>

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print + hard flush: AROS stdio to a file may ignore _IONBF, so flush
 * explicitly after every marker -- the last line in the file is then exactly
 * the last statement that executed. */
#define MARK(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

/* ---- guest monotonic clock, in microseconds -------------------------- */
static long mono_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (long)ts.tv_sec * 1000000L + (long)ts.tv_nsec / 1000L;
}

/* ---- a private timer.device request: real wall-clock ground truth ----- */
struct TimerCtx {
    struct MsgPort     *mp;
    struct timerequest *io;
    int ok;
};

static void timer_open(struct TimerCtx *t)
{
    t->ok = 0;
    t->mp = CreateMsgPort();
    if (!t->mp) return;
    t->io = (struct timerequest *)CreateIORequest(t->mp, sizeof(struct timerequest));
    if (!t->io) { DeleteMsgPort(t->mp); t->mp = NULL; return; }
    if (OpenDevice((STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)t->io, 0) != 0) {
        DeleteIORequest((struct IORequest *)t->io);
        DeleteMsgPort(t->mp);
        t->mp = NULL; t->io = NULL;
        return;
    }
    t->ok = 1;
}

static void timer_close(struct TimerCtx *t)
{
    if (!t->ok) return;
    CloseDevice((struct IORequest *)t->io);
    DeleteIORequest((struct IORequest *)t->io);
    DeleteMsgPort(t->mp);
    t->ok = 0;
}

/* Block on timer.device for ms milliseconds. This is driven by the host clock,
 * so it reflects REAL elapsed time regardless of the guest software counter. */
static void timer_wait(struct TimerCtx *t, int ms)
{
    if (!t->ok) return;
    t->io->tr_node.io_Command = TR_ADDREQUEST;
    t->io->tr_time.tv_secs  = ms / 1000;
    t->io->tr_time.tv_micro = (ms % 1000) * 1000;
    DoIO((struct IORequest *)t->io);
}

/* ---- spinner threads ------------------------------------------------- */
static volatile int spin_stop;
static volatile unsigned long spin_iters;

static void lower_pri(int pri)
{
    struct Task *me = FindTask(NULL);
    if (me) SetTaskPri(me, pri);
}

/* B: tight clock_gettime loop, never blocks. */
static void *spin_clock(void *arg)
{
    struct timespec ts;
    (void)arg;
    lower_pri(-1);
    while (!spin_stop) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        spin_iters++;
    }
    return NULL;
}

/* C: pthread_cond_timedwait(1ms) loop -- the app's idle-worker pattern. */
static void *spin_condwait(void *arg)
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
    (void)arg;
    lower_pri(-1);
    while (!spin_stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000000L;                 /* +1ms */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&m);
        pthread_cond_timedwait(&c, &m, &ts);    /* nobody signals c */
        pthread_mutex_unlock(&m);
        spin_iters++;
    }
    return NULL;
}

/* sched_yield loop -- crossbeam Backoff::snooze pattern. */
static void *spin_yield(void *arg)
{
    (void)arg;
    lower_pri(-1);
    while (!spin_stop) { sched_yield(); spin_iters++; }
    return NULL;
}

/* D: flips a flag after the spinner starts; measures whether a pri 0 task that
 * becomes READY actually gets to run while a pri -1 task is spinning. */
static volatile int d_ran;
static void *spin_forever(void *arg)
{
    (void)arg;
    lower_pri(-1);
    while (!spin_stop) { spin_iters++; }
    return NULL;
}

/* ---- test drivers ---------------------------------------------------- */

/* Sample the guest clock across 5 real 100ms timer.device waits. If the guest
 * clock tracks the timer (~100ms per step), PASS. If it barely moves while the
 * timer waits really elapse, the guest clock is FROZEN under load. */
static void run_clock_probe(struct TimerCtx *t, const char *label)
{
    long g0, g1;
    int step;
    int frozen_steps = 0;

    printf("   %-10s guest-clock deltas over 5 x 100ms timer.device waits:\n", label);
    printf("      ");
    for (step = 0; step < 5; step++) {
        g0 = mono_us();
        timer_wait(t, 100);        /* real 100ms of host time */
        g1 = mono_us();
        long d = g1 - g0;          /* guest us that "passed" during a real 100ms */
        printf("%ld ", d); fflush(stdout);
        if (d < 50000L) frozen_steps++;   /* < 50ms guest for 100ms real = frozen-ish */
    }
    printf("us\n");
    printf("      verdict: %s\n",
           frozen_steps >= 3 ? "*** GUEST CLOCK FROZEN UNDER LOAD ***"
                             : "ok (clock tracks real time)");
}

int main(int argc, char **argv)
{
    struct TimerCtx t;
    pthread_t th;
    const char *variant = (argc > 1) ? argv[1] : "clock";
    void *(*spinfn)(void *) = spin_clock;

    if (!strcmp(variant, "pure"))  spinfn = spin_forever;   /* raw arithmetic, no syscall */
    else if (!strcmp(variant, "yield")) spinfn = spin_yield; /* sched_yield loop */
    else if (!strcmp(variant, "cond"))  spinfn = spin_condwait;
    /* default "clock": tight clock_gettime loop (Disable/Enable => masks SIGALRM) */

    printf("ClockTest variant=%s\n", variant);
    printf("Q: while a pri -1 thread runs this spin, does a pri 0 timer.device wait wake?\n\n");
    fflush(stdout);

    timer_open(&t);
    if (!t.ok) { printf("FATAL: could not open timer.device\n"); return 1; }

    MARK("[A] baseline 200ms timer wait (no spinner)...\n");
    { long g0 = mono_us(); timer_wait(&t, 200); long g1 = mono_us();
      MARK("[A] returned; guest clock moved %ld us  %s\n", g1 - g0,
           (g1 - g0) >= 100000L ? "ok" : "*** clock slow/frozen ***"); }

    MARK("[B] spawning pri -1 '%s' spinner...\n", variant);
    spin_stop = 0; spin_iters = 0;
    pthread_create(&th, NULL, spinfn, NULL);

    /* Give the spinner a moment to reach pri -1 and start. We must NOT block on
     * the timer here (that is the thing under test), so spin-yield briefly. */
    { int k; for (k = 0; k < 200000; k++) sched_yield(); }

    MARK("[C] spinner is running (%lu iters). main -> 200ms timer.device wait...\n", spin_iters);
    { long g0 = mono_us(); unsigned long i0 = spin_iters;
      timer_wait(&t, 200);                      /* THE TEST: does this ever return? */
      long g1 = mono_us(); unsigned long spun = spin_iters - i0;
      MARK("[D] timer wait RETURNED. guest clock moved %ld us, spinner +%lu iters  %s\n",
           g1 - g0, spun,
           (g1 - g0) >= 100000L ? "ok (timer + clock survived the spinner)"
                                : "*** clock frozen but timer woke ***"); }

    spin_stop = 1;
    pthread_join(th, NULL);
    timer_close(&t);
    MARK("done.\n");
    return 0;
}
