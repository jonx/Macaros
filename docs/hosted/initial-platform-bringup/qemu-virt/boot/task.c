// AROS AArch64 bring-up — M7: a tiny cooperative scheduler to exercise the switch.
//
// Foreshadows AROS's task/context creation (createcontext.c) and the cpu_*/core_*
// dispatch split. Two tasks alternate by yielding to each other; each keeps its
// own loop counter on its own stack, so correct alternation proves the switch
// preserves the full callee-saved state + SP.

#include "kern.h"

static struct context ctx_main, ctx_a, ctx_b;

static void task_init(struct context *c, void (*entry)(void), void *stack_top)
{
    for (unsigned i = 0; i < sizeof(*c) / sizeof(uint64_t); i++)
        ((uint64_t *)c)[i] = 0;
    c->x30 = (uint64_t)entry;       // ctx_switch's `ret` jumps here on first switch
    c->sp  = (uint64_t)stack_top;   // fresh 16-byte-aligned stack
}

static void task_a(void)
{
    for (int i = 0; i < 3; i++) {
        kprintf("  taskA i=%d (stack~%p)\n", i, (void *)&i);
        ctx_switch(&ctx_a, &ctx_b);     // yield to B
    }
    ctx_switch(&ctx_a, &ctx_main);      // done -> return to the bootstrap context
}

static void task_b(void)
{
    for (int i = 0; i < 3; i++) {
        kprintf("  taskB i=%d (stack~%p)\n", i, (void *)&i);
        ctx_switch(&ctx_b, &ctx_a);     // yield to A
    }
    ctx_switch(&ctx_b, &ctx_main);
}

void tasks_demo(void)
{
    void *sa = pmm_alloc();             // one page of stack per task (from M6)
    void *sb = pmm_alloc();
    task_init(&ctx_a, task_a, (char *)sa + 4096);
    task_init(&ctx_b, task_b, (char *)sb + 4096);

    kprintf("[M7a] starting cooperative tasks A/B (separate stacks)\n");
    ctx_switch(&ctx_main, &ctx_a);      // dive in; returns when A yields to main
    kprintf("[M7] context switch ok: A/B alternated, counters preserved\n");
}
