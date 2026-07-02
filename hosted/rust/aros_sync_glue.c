/* aros_sync_glue.c -- pthread mutex/cond for the Rust std sync pal
 * (sys/pal/unsupported/sync/{mutex,condvar}.rs, used by the pthread Mutex/Condvar
 * wrappers once std::thread is wired).
 *
 * Like the fs glue this includes the real AROS <pthread.h> (via -I gen/include/
 * aros/posixc, not the macOS SDK), so C owns the opaque pthread_mutex_t /
 * pthread_cond_t (SignalSemaphore-based; sizes 136 / 152 bytes on aarch64). The Rust
 * side keeps a zeroed byte buffer of that size and passes its pointer; a zeroed
 * buffer is a valid PTHREAD_*_INITIALIZER, and we also init the mutex explicitly to
 * PTHREAD_MUTEX_NORMAL (deadlock-on-relock, matching std's unix pal). -ffixed-x18.
 *
 * Independent work: from the AROS pthread.library headers/autodocs only.
 */
#include <pthread.h>
#include <time.h>

unsigned long aros_mtx_size(void)  { return (unsigned long)sizeof(pthread_mutex_t); }
unsigned long aros_cond_size(void) { return (unsigned long)sizeof(pthread_cond_t); }

/* Explicit NORMAL-type init (so re-locking from the same thread deadlocks rather
 * than being UB), matching std's unix Mutex::init. */
int aros_mtx_init(void *m)
{
    pthread_mutexattr_t attr;
    int r;
    if (pthread_mutexattr_init(&attr) != 0)
        return -1;
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    r = pthread_mutex_init((pthread_mutex_t *)m, &attr);
    pthread_mutexattr_destroy(&attr);
    return r;
}
int aros_mtx_lock(void *m)    { return pthread_mutex_lock((pthread_mutex_t *)m); }
int aros_mtx_trylock(void *m) { return pthread_mutex_trylock((pthread_mutex_t *)m); }
int aros_mtx_unlock(void *m)  { return pthread_mutex_unlock((pthread_mutex_t *)m); }
int aros_mtx_destroy(void *m) { return pthread_mutex_destroy((pthread_mutex_t *)m); }

int aros_cond_init(void *c)      { return pthread_cond_init((pthread_cond_t *)c, (const pthread_condattr_t *)0); }
int aros_cond_signal(void *c)    { return pthread_cond_signal((pthread_cond_t *)c); }
int aros_cond_broadcast(void *c) { return pthread_cond_broadcast((pthread_cond_t *)c); }
int aros_cond_wait(void *c, void *m)
{
    return pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m);
}
int aros_cond_destroy(void *c)   { return pthread_cond_destroy((pthread_cond_t *)c); }

/* Relative timed wait: compute the absolute deadline here (REALTIME, the pthread
 * default clock) so Rust doesn't need the timespec layout. Returns 0 if signalled,
 * ETIMEDOUT on timeout, other errno on error. */
int aros_cond_timedwait(void *c, void *m, unsigned int secs, unsigned int nsecs)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return -1;
    /* AROS time_t is a signed 32-bit int. Rust passes "forever" as u32::MAX seconds,
     * which would wrap the deadline negative and make pthread_cond_timedwait return
     * ETIMEDOUT immediately (the caller then spins at 100% CPU re-waiting). Clamp the
     * deadline to time_t's range: a year-2038 deadline is a decade of waiting, which
     * is "forever" for a condvar; the std wrapper re-loops if it ever fires early. */
    {
        long max_add = 0x7FFFFFFFL - 1 - (long)ts.tv_sec;
        if (max_add < 0) max_add = 0;
        if ((long)secs > max_add) secs = (unsigned int)max_add;
    }
    ts.tv_sec += (time_t)secs;
    ts.tv_nsec += (long)nsecs;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait((pthread_cond_t *)c, (pthread_mutex_t *)m, &ts);
}
