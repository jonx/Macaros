// AROS AArch64 bring-up — boot orchestration.
//
// start.S hands control here once the stack is up and .bss is zeroed. kmain walks
// the Phase-1 milestones in order, each gated by a serial marker the harness
// checks. The subsystems live in their own files (uart/exc/mmu/irq/pmm/task/fb);
// this is just the sequence that exercises them.

#include <stdint.h>
#include "kern.h"

static unsigned current_el(void) {
    unsigned long v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (unsigned)((v >> 2) & 3);   // CurrentEL[3:2]
}

// x0_at_entry is whatever the boot path left in x0; under QEMU's ELF -kernel it is
// 0 (NOT the DTB — see start.S / HARDWARE.md). Printed so the fact stays verified.
void kmain(unsigned long x0_at_entry) {
    uart_init();
    kprintf("[M2] hello from C (EL%u) x0=%p\n", current_el(), (void *)x0_at_entry);

    // M3: install exception vectors, then deliberately trap to prove they work.
    vectors_init();
    kprintf("[M3a] VBAR set, triggering traps\n");
    __asm__ volatile("svc #0");     // -> sync exception, EC=0x15, resumes after
    __asm__ volatile("brk #0");     // -> sync exception, EC=0x3c, handler skips it
    kprintf("[M3] vectors ok\n");

    // M4: enable the MMU. The print AFTER proves the identity map is correct and
    // we didn't vanish; then a deliberate unmapped access proves translation runs.
    kprintf("[M4a] enabling MMU (identity map)...\n");
    mmu_init();
    __asm__ volatile("ldr x9, [%0]" :: "r"(0x80000000UL) : "x9", "memory");  // -> fault, FAR
    kprintf("[M4] MMU verified (SCTLR.M=%u, identity map + translation fault), alive\n",
            (unsigned)(SYSREG_READ("sctlr_el1") & 1));

    // M5: first asynchronous event — GICv2 + EL1 physical timer at 100 Hz.
    kprintf("[M5a] init GICv2 + EL1 phys timer (INTID 30)\n");
    gic_init();
    timer_init(100);
    irqs_enable();
    while (timer_ticks < 5)
        __asm__ volatile("wfi");
    kprintf("[M5] timer IRQ ok, ticks=%lu\n", timer_ticks);

    // M6: physical page allocator. Prove alloc, read/write, free, LIFO reuse.
    kprintf("[M6a] pmm init (heap above _end)\n");
    pmm_init();
    uint64_t total = pmm_free_count();
    void *a = pmm_alloc(), *b = pmm_alloc(), *c = pmm_alloc();
    *(volatile uint64_t *)a = 0xA5A5A5A5A5A5A5A5UL;
    *(volatile uint64_t *)c = 0xC3C3C3C3C3C3C3C3UL;
    int rw = (*(volatile uint64_t *)a == 0xA5A5A5A5A5A5A5A5UL)
          && (*(volatile uint64_t *)c == 0xC3C3C3C3C3C3C3C3UL);
    pmm_free(b);
    void *d = pmm_alloc();           // LIFO -> should hand back b
    kprintf("[M6] pmm ok: total=%lu pages a=%p b=%p c=%p rw=%d reuse=%d free=%lu\n",
            total, a, b, c, rw, (d == b), pmm_free_count());

    // M7: cooperative context switch between two tasks on separate stacks.
    tasks_demo();

    // M8: minimal shell — reads injected keystrokes from the UART, dispatches.
    shell_run();

    // M9: framebuffer via ramfb (stays up through the M10 window for the screendump).
    fb_init();

    // M10: preemptive multitasking — the timer IRQ switches tasks (no yields).
    sched_demo();
}
