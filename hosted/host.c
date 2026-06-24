// Hosted AArch64 AROS — Phase 2 spike, step 1 (H0/H1).
//
// Runs as a NATIVE arm64 macOS process. The whole premise of the hosted port is
// "macOS owns the drivers": here the console is macOS stdout and memory is macOS
// malloc — AROS-side code never touches hardware. What we must prove is that the
// AROS-side machinery (starting with the context switch we built bare-metal)
// works unchanged at EL0 in a process. That's H1.
//
// This is the cheap de-risking probe before committing to the full hosted port:
// if our ctx_switch drives tasks here — including running host calls (printf) on
// a switched, host-allocated stack — the hosted architecture is sound.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Must match hosted/switch.S offsets.
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, sp;
};
extern void ctx_switch(struct context *old, struct context *next);

static struct context ctx_main, ctx_a, ctx_b;
static int a_count, b_count;

static void task_init(struct context *c, void (*entry)(void), void *stack_top) {
    memset(c, 0, sizeof(*c));
    c->x30 = (uint64_t)entry;       // ctx_switch's `ret` enters here
    c->sp  = (uint64_t)stack_top;   // host-allocated stack, 16-byte aligned
}

static void task_a(void) {
    for (int i = 0; i < 3; i++) {
        printf("[H1] task A iter %d (hosted on macOS, host console)\n", i);
        a_count++;
        ctx_switch(&ctx_a, &ctx_b);     // yield to B
    }
    ctx_switch(&ctx_a, &ctx_main);      // done -> back to main
}

static void task_b(void) {
    for (int i = 0; i < 3; i++) {
        printf("[H1] task B iter %d (hosted on macOS, host console)\n", i);
        b_count++;
        ctx_switch(&ctx_b, &ctx_a);     // yield to A
    }
    ctx_switch(&ctx_b, &ctx_main);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered: output survives if we abort
    printf("[H0] hosted AArch64 AROS spike alive (native arm64 macOS process)\n");

    const size_t STK = 1 << 16;         // 64 KiB host-provided stacks
    void *sa = malloc(STK), *sb = malloc(STK);
    task_init(&ctx_a, task_a, (char *)sa + STK);
    task_init(&ctx_b, task_b, (char *)sb + STK);

    ctx_switch(&ctx_main, &ctx_a);      // dive in; returns when A yields to main

    printf("[H1] hosted context switch ok: A=%d B=%d (macOS owns console+memory)\n",
           a_count, b_count);
    free(sa); free(sb);
    return 0;
}
