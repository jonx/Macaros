/* aros_thread_glue.c -- flat-C boundary between Rust `std::thread` and AROS
 * pthread.library (the -lpthread linklib). std's `sys/thread/aros.rs` calls these
 * aros_thr_* wrappers; the TLS-key side (pthread_key_*) is called from Rust
 * directly (sys/thread_local/key/aros.rs) because those take no opaque structs.
 *
 * Only the spawn path needs C: it owns the opaque `pthread_attr_t` (so we can set
 * the stack size Rust asks for) without Rust having to know its layout. Header-clean
 * like the other glues -- <pthread.h> can drag the macOS SDK on this backend
 * (UPSTREAM-NOTES #34), so we redeclare the handful of pthread bits we use, matching
 * compiler/pthread/pthread.h exactly (aarch64, non-MorphOS: sched_param is one int,
 * so pthread_attr is 32 bytes). Compiled with -ffixed-x18 like the net glue.
 *
 * Independent work: written from the AROS pthread.library headers/autodocs; no
 * third-party source consulted.
 */
#include <proto/dos.h>   /* Delay() for aros_thr_sleep -- no <time.h> */

typedef unsigned int pthread_t;

struct sched_param { int sched_priority; };
struct pthread_attr {
    void  *stackaddr;
    unsigned long stacksize;
    int    detachstate;
    struct sched_param param;
    int    inheritsched;
    int    contentionscope;
};
typedef struct pthread_attr pthread_attr_t;

extern int  pthread_attr_init(pthread_attr_t *attr);
extern int  pthread_attr_destroy(pthread_attr_t *attr);
extern int  pthread_attr_setstacksize(pthread_attr_t *attr, unsigned long stacksize);
extern int  pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start)(void *), void *arg);
extern int  pthread_join(pthread_t thread, void **value_ptr);
extern int  pthread_detach(pthread_t thread);
extern int  sched_yield(void);

/* Spawn a joinable thread with the requested stack size (0 = library default).
 * Writes the thread id to *out_tid and returns 0, or a pthread errno on failure. */
int aros_thr_spawn(unsigned long stacksize, void *(*start)(void *), void *arg,
                   unsigned int *out_tid)
{
    pthread_attr_t attr;
    pthread_t tid = 0;
    int rc;

    if (!start || !out_tid)
        return 22 /* EINVAL */;

    if (pthread_attr_init(&attr) != 0)
        return 12 /* ENOMEM */;
    if (stacksize > 0)
        pthread_attr_setstacksize(&attr, stacksize);

    rc = pthread_create(&tid, &attr, start, arg);
    pthread_attr_destroy(&attr);

    if (rc == 0)
        *out_tid = tid;
    return rc;
}

int aros_thr_join(unsigned int tid)
{
    return pthread_join((pthread_t)tid, (void **)0);
}

int aros_thr_detach(unsigned int tid)
{
    return pthread_detach((pthread_t)tid);
}

void aros_thr_yield(void)
{
    sched_yield();
}

/* thread::sleep, via dos Delay (50 ticks/sec = 20ms granularity). Coarse but
 * robust; blocks only the calling task, so other threads keep running. */
int aros_thr_sleep(unsigned int secs, unsigned int nsecs)
{
    unsigned long ticks = (unsigned long)secs * 50UL + (unsigned long)(nsecs / 20000000U);
    if (ticks == 0 && (secs || nsecs))
        ticks = 1;              /* round any nonzero sub-20ms request up to one tick */
    /* Delay() takes a 32-bit ULONG; secs=u32::MAX (Rust's "sleep forever" chunk) is
     * ~214e9 ticks, which would truncate mod 2^32 and wake years early. Chunk it. */
    while (ticks > 0xFFFFFFFFUL) {
        Delay(0xFFFFFFFFUL);
        ticks -= 0xFFFFFFFFUL;
    }
    if (ticks > 0)
        Delay(ticks);
    return 0;
}
