/*
 * TimerTest -- do AROS's timed-wait primitives actually WAIT?
 *
 * Built to answer one question that hours of inference could not: when a
 * thread asks to sleep for N ms, does it sleep N ms, or return immediately?
 * An immediate return turns every "wait with a timeout, retry" loop -- which
 * is what Rust's Thread::park_timeout(), crossbeam's recv_timeout() and every
 * idle thread-pool worker do -- into a 100% CPU spin. On hosted AROS every
 * task shares ONE guest CPU, so one spinning worker wedges the whole machine:
 * that is the "Feraille freezes after a few clicks / while scrolling" report.
 *
 * No gpui, no Rust, no app: just the primitives.
 *
 * Test 1  pthread_cond_timedwait() on a condvar nobody signals, over a range
 *         of timeouts. This is the exact path Rust's park_timeout() takes
 *         (std condvar -> aros_cond_timedwait -> pthread_cond_timedwait), and
 *         the deadline is built the same way (clock_gettime(CLOCK_REALTIME)
 *         + delta), so a mismatch between that clock and the gettimeofday()
 *         pthread_cond_timedwait uses internally shows up here.
 * Test 2  usleep()/nanosleep() -- posixc's timer path (opens/closes
 *         timer.device per call).
 * Test 3  sched_yield() rate -- crossbeam's Backoff::snooze() calls this in a
 *         spin loop; it must be cheap.
 * Test 4  the same timed waits with N threads running concurrently, which is
 *         the state the app is actually in (workers + timer thread + UI).
 *
 * A PASS means measured >= 70% of requested. Anything near 0 ms is the bug.
 *
 * Usage: TimerTest [THREADS n]
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <exec/types.h>
#include <devices/timer.h>

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- wall clock (CLOCK_MONOTONIC, the clock Rust's Instant uses) --------
 * Integer microseconds, and every result below prints as an integer: AROS's
 * printf renders %f/%g verbatim (UPSTREAM-NOTES item 34), so floats would come
 * out as the literal format string. */
static long now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (long)ts.tv_sec * 1000000L + (long)ts.tv_nsec / 1000L;
}

/* ---- Test 1/4: pthread_cond_timedwait, exactly as Rust reaches it ------- */
static long timed_wait_once(int ms)
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
    struct timespec ts;
    long t0, t1;
    int rc;

    /* Deadline built the same way aros_cond_timedwait() builds it. */
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return -1;
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&m);
    t0 = now_us();
    rc = pthread_cond_timedwait(&c, &m, &ts);   /* nobody ever signals c */
    t1 = now_us();
    pthread_mutex_unlock(&m);

    (void)rc;   /* expected: ETIMEDOUT */
    return t1 - t0;
}

static void test_cond_timedwait(const char *tag)
{
    static const int cases[] = { 1, 2, 5, 16, 50, 100 };
    unsigned i;

    printf("\n== %s: pthread_cond_timedwait (Rust park_timeout path) ==\n", tag);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int ms = cases[i];
        long got = timed_wait_once(ms);
        const char *verdict = (got < 0) ? "ERROR"
                            : (got >= (long)ms * 700L) ? "ok"
                            : (got < 1000L) ? "*** RETURNED IMMEDIATELY ***"
                                            : "*** TOO SHORT ***";
        printf("   asked %-4d ms   measured %7ld us   %s\n", ms, got, verdict);
    }
}

/* ---- Test 2: posixc sleeps ---------------------------------------------- */
static void test_posix_sleeps(void)
{
    long t0, t1;
    struct timespec req;

    printf("\n== usleep / nanosleep (posixc timer path) ==\n");

    t0 = now_us(); usleep(2000); t1 = now_us();
    printf("   usleep(2000us)    measured %7ld us   %s\n", t1 - t0,
           (t1 - t0) >= 1400L ? "ok" : "*** TOO SHORT ***");

    req.tv_sec = 0; req.tv_nsec = 16 * 1000000L;
    t0 = now_us(); nanosleep(&req, NULL); t1 = now_us();
    printf("   nanosleep(16ms)   measured %7ld us   %s\n", t1 - t0,
           (t1 - t0) >= 11200L ? "ok" : "*** TOO SHORT ***");
}

/* ---- Test 3: sched_yield cost ------------------------------------------- */
static void test_yield_rate(void)
{
    const int N = 20000;
    long t0, t1, total;
    int i;

    printf("\n== sched_yield (crossbeam Backoff::snooze path) ==\n");
    t0 = now_us();
    for (i = 0; i < N; i++)
        sched_yield();
    t1 = now_us();
    total = t1 - t0;
    printf("   %d yields in %ld us => %ld ns each\n",
           N, total, (total * 1000L) / (long)N);
}

/* ---- Test 4: concurrent timed waits ------------------------------------- */
static volatile int conc_bad;

static void *conc_thread(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 20; i++)
    {
        long got = timed_wait_once(16);
        if (got >= 0 && got < 1000L)
            conc_bad++;
    }
    return NULL;
}

static void test_concurrent(int nthreads)
{
    pthread_t th[16];
    long t0, t1;
    int i, n = nthreads > 16 ? 16 : nthreads;

    printf("\n== %d concurrent threads x 20 x cond_timedwait(16ms) ==\n", n);
    conc_bad = 0;
    t0 = now_us();
    for (i = 0; i < n; i++)
        pthread_create(&th[i], NULL, conc_thread, NULL);
    for (i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    t1 = now_us();

    /* 20 sequential 16ms waits per thread => >= 320ms wall if they really wait
     * (all threads share one guest CPU, but sleeping is concurrent). */
    printf("   wall %ld us (expect >= ~320000), immediate-returns: %d  %s\n",
           t1 - t0, conc_bad,
           (conc_bad == 0 && (t1 - t0) >= 224000L) ? "ok"
                                                   : "*** SPINNING / NOT WAITING ***");
}

int main(int argc, char **argv)
{
    int nthreads = 4;
    int i;

    for (i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "THREADS"))
            nthreads = atoi(argv[i + 1]);

    printf("TimerTest -- do AROS timed waits actually wait?\n");
    printf("(a measured time near 0 for a nonzero request is THE bug)\n");

    test_cond_timedwait("single-threaded");
    test_posix_sleeps();
    test_yield_rate();
    test_concurrent(nthreads);

    printf("\ndone.\n");
    return 0;
}
