/* Guest-memory crossings for timer.device's direct-call vectors. */
#include "emu68k_internal.h"

#include <stdio.h>
#include <time.h>

#define TIMER_LVO_ADDTIME      7
#define TIMER_LVO_SUBTIME      8
#define TIMER_LVO_CMPTIME      9
#define TIMER_LVO_READECLOCK  10
#define TIMER_LVO_GETSYSTIME  11
#define TIMER_LVO_GETUPTIME   12

#define AMIGA_UNIX_EPOCH_DELTA 252460800u
#define ECLOCK_FREQUENCY       1000000u

static int guest_span_ok(j4_sandbox *sb, uint32_t addr, uint32_t size)
{
    return addr >= sb->sandbox_origin &&
           (uint64_t)addr + size <=
               (uint64_t)sb->sandbox_origin + sb->size;
}

static int timeval_ok(j4_sandbox *sb, uint32_t addr)
{
    return guest_span_ok(sb, addr, 8u);
}

static void normalize(uint32_t *secs, uint32_t *micro)
{
    *secs += *micro / 1000000u;
    *micro %= 1000000u;
}

static void write_clock(j4_sandbox *sb, uint32_t dest, clockid_t clock_id,
                        int amiga_epoch)
{
    struct timespec ts;
    uint64_t seconds;
    clock_gettime(clock_id, &ts);
    seconds = (uint64_t)ts.tv_sec;
    if (amiga_epoch && seconds >= AMIGA_UNIX_EPOCH_DELTA)
        seconds -= AMIGA_UNIX_EPOCH_DELTA;
    gwrite32(sb, dest, (uint32_t)seconds);
    gwrite32(sb, dest + 4u, (uint32_t)(ts.tv_nsec / 1000));
}

int emu68k_timerdevice_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                            struct j5d_m68k_state *st, char *e, unsigned el)
{
    uint32_t dest = st->a[0], src = st->a[1];
    uint32_t ds, du, ss, su;
    (void)r;

    switch (lvo) {
    case TIMER_LVO_ADDTIME:
        if (!timeval_ok(sb, dest) || !timeval_ok(sb, src)) goto bad_timeval;
        ds = gread32(sb, dest); du = gread32(sb, dest + 4u);
        ss = gread32(sb, src);  su = gread32(sb, src + 4u);
        ds += ss; du += su; normalize(&ds, &du);
        gwrite32(sb, dest, ds); gwrite32(sb, dest + 4u, du);
        return 0;

    case TIMER_LVO_SUBTIME:
        if (!timeval_ok(sb, dest) || !timeval_ok(sb, src)) goto bad_timeval;
        ds = gread32(sb, dest); du = gread32(sb, dest + 4u);
        ss = gread32(sb, src);  su = gread32(sb, src + 4u);
        normalize(&ds, &du); normalize(&ss, &su);
        if (du < su) { du += 1000000u; ds--; }
        gwrite32(sb, dest, ds - ss);
        gwrite32(sb, dest + 4u, du - su);
        /* AROS normalizes src as part of SubTime's historical contract. */
        gwrite32(sb, src, ss); gwrite32(sb, src + 4u, su);
        return 0;

    case TIMER_LVO_CMPTIME:
        if (!timeval_ok(sb, dest) || !timeval_ok(sb, src)) goto bad_timeval;
        ds = gread32(sb, dest); du = gread32(sb, dest + 4u);
        ss = gread32(sb, src);  su = gread32(sb, src + 4u);
        st->d[0] = (ds > ss || (ds == ss && du > su)) ? UINT32_MAX :
                    (ds < ss || (ds == ss && du < su)) ? 1u : 0u;
        return 0;

    case TIMER_LVO_READECLOCK: {
        struct timespec ts;
        uint64_t ticks;
        if (!guest_span_ok(sb, dest, 8u)) goto bad_timeval;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ticks = (uint64_t)ts.tv_sec * ECLOCK_FREQUENCY +
                (uint64_t)ts.tv_nsec / 1000u;
        gwrite32(sb, dest, (uint32_t)(ticks >> 32));
        gwrite32(sb, dest + 4u, (uint32_t)ticks);
        st->d[0] = ECLOCK_FREQUENCY;
        return 0;
    }

    case TIMER_LVO_GETSYSTIME:
        if (!timeval_ok(sb, dest)) goto bad_timeval;
        write_clock(sb, dest, CLOCK_REALTIME, 1);
        return 0;

    case TIMER_LVO_GETUPTIME:
        if (!timeval_ok(sb, dest)) goto bad_timeval;
        write_clock(sb, dest, CLOCK_MONOTONIC, 0);
        return 0;

    default:
        return 1;
    }

bad_timeval:
    snprintf(e, el, "timer.device LVO %d received a timeval outside guest memory",
             lvo);
    return 1;
}
