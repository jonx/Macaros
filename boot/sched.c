// AROS AArch64 bring-up — M10: preemptive multitasking.
//
// The timer IRQ (M5) now drives task switching. schedule() is called from
// irq_dispatch on every tick; it saves the preempted task's full frame and
// returns the next task's frame, which vectors.S restores + ERETs into. The demo
// tasks run infinite loops with NO cooperative yield, so the fact that all of
// them make progress proves the timer is preempting them. Foreshadows AROS's
// core_Schedule/cpu_Switch driven from the system timer.

#include "kern.h"

#define NTASK 3                 // [0] = main (this context), [1] = A, [2] = B

struct task { uint64_t saved_sp; };
static struct task tasks[NTASK];
static int cur = 0;
static volatile int preempt_on = 0;

// Called from the timer IRQ. Round-robin; returns the frame to resume.
struct trapframe *schedule(struct trapframe *tf)
{
    if (!preempt_on)
        return tf;
    tasks[cur].saved_sp = (uint64_t)tf;             // save preempted task's frame
    cur = (cur + 1) % NTASK;
    return (struct trapframe *)tasks[cur].saved_sp;  // resume the next
}

// Manufacture an initial frame so the first restore+ERET enters `entry` at EL1h
// with interrupts enabled (so the task is itself preemptible).
static void task_spawn(int slot, void (*entry)(void), void *stack_top)
{
    struct trapframe *f = (struct trapframe *)((char *)stack_top - sizeof(struct trapframe));
    for (unsigned i = 0; i < sizeof(*f) / sizeof(uint64_t); i++)
        ((uint64_t *)f)[i] = 0;
    f->elr  = (uint64_t)entry;
    f->spsr = 0x5;                                   // M=EL1h (0b0101), DAIF=0 (IRQs on)
    tasks[slot].saved_sp = (uint64_t)f;
}

static volatile uint64_t ran_a, ran_b;

static void task_a(void)
{
    for (;;) {
        if (ran_a < 3) kprintf("[M10] task A run #%lu (preempted, never yields)\n", ran_a + 1);
        ran_a++;
        for (volatile int i = 0; i < 40000; i++) { }   // burn time; the timer preempts us
    }
}

static void task_b(void)
{
    for (;;) {
        if (ran_b < 3) kprintf("[M10] task B run #%lu (preempted, never yields)\n", ran_b + 1);
        ran_b++;
        for (volatile int i = 0; i < 40000; i++) { }
    }
}

void sched_demo(void)
{
    void *sa = pmm_alloc();
    void *sb = pmm_alloc();
    task_spawn(1, task_a, (char *)sa + 4096);
    task_spawn(2, task_b, (char *)sb + 4096);
    cur = 0;                                          // tasks[0] = main (frame captured on 1st tick)

    kprintf("[M10a] preemption armed (timer-driven, 3-way round robin)\n");
    preempt_on = 1;

    uint64_t target = timer_ticks + 250;              // ~2.5s window (also covers screendump)
    while (timer_ticks < target)
        __asm__ volatile("wfi");                      // timer preempts main <-> A <-> B

    preempt_on = 0;
    kprintf("[M10] preemptive multitasking ok: A ran=%lu B ran=%lu (no yields)\n",
            ran_a, ran_b);
}
