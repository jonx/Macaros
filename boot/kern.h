// AROS AArch64 bring-up — shared kernel declarations.
// Small on purpose; grows as milestones land.
#ifndef KERN_H
#define KERN_H

#include <stdint.h>

// PL011 UART console + printf-to-UART (M2/M8); defined in uart.c.
void uart_init(void);
void uart_putc(char c);
int  uart_getc(void);
void kprintf(const char *fmt, ...);

// Minimal line shell (M8); defined in shell.c.
void shell_run(void);

// Exception vectors (M3): install VBAR_EL1; defined in exc.c / vectors.S.
void vectors_init(void);

// Full trap frame saved by vectors.S (offsets are hardcoded there: x0..x30 at
// 0..240, elr at 256, spsr at 264). The handler may return a DIFFERENT frame to
// resume another task (preemption, M10).
struct trapframe {
    uint64_t x[31];   // x0..x30
    uint64_t _pad;
    uint64_t elr;
    uint64_t spsr;
};
struct trapframe *exc_handler(struct trapframe *tf, unsigned long kind);

// MMU (M4): build an identity map and enable translation; defined in mmu.c.
void mmu_init(void);

// Interrupts + timer (M5): GICv2 + EL1 physical timer; defined in irq.c.
void gic_init(void);
void timer_init(unsigned hz);
void irqs_enable(void);
struct trapframe *irq_dispatch(struct trapframe *tf);
extern volatile uint64_t timer_ticks;

// Preemptive scheduler (M10): defined in sched.c. schedule() is called from the
// timer IRQ and may return another task's frame; sched_demo runs the demo.
struct trapframe *schedule(struct trapframe *tf);
void sched_demo(void);

// Physical page allocator (M6): defined in pmm.c.
void pmm_init(void);
void *pmm_alloc(void);
void pmm_free(void *p);
uint64_t pmm_free_count(void);

// Cooperative context switch (M7): defined in switch.S / task.c.
// Layout MUST match the offsets hardcoded in switch.S.
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, sp;
};
void ctx_switch(struct context *old, struct context *next);
void tasks_demo(void);

// Framebuffer via ramfb (M9): defined in fb.c.
void fb_init(void);

// Read/write an AArch64 system register by name, e.g. SYSREG_READ("esr_el1").
#define SYSREG_READ(reg) ({ uint64_t _v; __asm__ volatile("mrs %0, " reg : "=r"(_v)); _v; })
#define SYSREG_WRITE(reg, val) \
    __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(val)) : "memory")

#endif /* KERN_H */
