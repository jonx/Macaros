/*
 * x18probe.c -- does macOS preserve a user value in x18 across signal delivery?
 *
 * Background: AROS-hosted preempts tasks with signals. On aarch64 it saves and
 * restores the interrupted register set (x0..x28 incl. x18) from the signal
 * context. On Linux/bare-metal that keeps x18 usable. On Apple Silicon x18 is the
 * platform register the macOS kernel reserves for itself, so the question is
 * whether the signal frame macOS hands us even contains the user's x18.
 *
 * This probe mimics the AROS preemption exactly: hold a sentinel in x18, let a
 * timer signal fire while we hold it, and read x18 from the signal context -- the
 * very value AROS's SAVEREGS would copy.
 *
 *   cc -arch arm64 -O0 -D_XOPEN_SOURCE -Wno-deprecated-declarations \
 *      -Wno-inline-asm x18probe.c -o x18probe && ./x18probe
 *
 * Observed on macOS 15 / Apple M-series, every run:
 *   user put in x18      : 0xCAFEF00DDEADBEEF
 *   x18 in signal context: 0x0000000000000000
 *   verdict              : CLOBBERED -> macOS wiped it before AROS sees it
 *
 * Conclusion: macOS zeroes x18 in the signal frame, so the host cannot preserve a
 * user x18 across a preemption no matter how faithfully it saves/restores. The
 * aarch64-AROS target must reserve x18 (-ffixed-x18). See ../../NOTES.md and
 * this folder's README.md.
 */
#define _XOPEN_SOURCE
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <stdint.h>

static volatile int got = 0;
static volatile uint64_t captured = 0;
static const uint64_t SENT = 0xCAFEF00DDEADBEEFULL;

static void handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    captured = uc->uc_mcontext->__ss.__x[18];
    got = 1;
}

int main(void) {
    struct sigaction sa;
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it = {{0, 0}, {0, 2000}};   /* fire once in ~2 ms */
    setitimer(ITIMER_REAL, &it, NULL);

    uint64_t sent = SENT;
    /* Spin holding x18 = sentinel; the loop body only touches x0, so x18 stays
       live until the timer signal interrupts us mid-loop. */
    asm volatile(
        "ldr x18, %[s]\n"
    "1:\n"
        "ldr w0, %[g]\n"
        "cbz w0, 1b\n"
        : : [s] "m"(sent), [g] "m"(got) : "x18", "x0", "memory");

    printf("user put in x18      : 0x%016llX\n", (unsigned long long)SENT);
    printf("x18 in signal context: 0x%016llX\n", (unsigned long long)captured);
    printf("verdict              : %s\n",
           captured == SENT ? "PRESERVED -> host save/restore would work, x18 usable"
                            : "CLOBBERED -> macOS wiped it before AROS sees it");
    return 0;
}
