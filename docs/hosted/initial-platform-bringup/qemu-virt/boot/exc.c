// AROS AArch64 bring-up — the C exception handler (M3) with frame-return (M10).
//
// Foreshadows AROS's intr.c (__vectorhand_*). vectors.S saves a full trap frame
// and calls exc_handler(frame, kind); we return the frame to resume — the SAME
// frame for a normal return, or (for a preemptive timer tick) another task's.
// We modify tf->elr in the FRAME rather than live ELR_EL1, since EXC_RESTORE
// reloads ELR/SPSR from the frame.
//
// EC encodings grounded from Linux esr.h: SVC64=0x15, BRK64=0x3C, DABT(cur)=0x25.

#include "kern.h"

enum { K_SYNC = 0, K_IRQ, K_FIQ, K_SERROR, K_INVALID };

struct trapframe *exc_handler(struct trapframe *tf, unsigned long kind)
{
    // Async interrupts: route to the dispatcher, which may preempt to another task.
    // (QEMU virt GICv2 may signal our group-0 timer as IRQ or FIQ; handle both.)
    if (kind == K_IRQ || kind == K_FIQ)
        return irq_dispatch(tf);

    uint64_t esr = SYSREG_READ("esr_el1");
    uint64_t far = SYSREG_READ("far_el1");
    unsigned ec  = (unsigned)((esr >> 26) & 0x3f);

    static const char *const kinds[] = { "SYNC", "IRQ", "FIQ", "SError", "INVALID" };
    kprintf("  exc kind=%s EC=0x%x ESR=0x%lx ELR=0x%lx SPSR=0x%lx FAR=0x%lx\n",
            kinds[kind <= K_INVALID ? kind : K_INVALID], ec, esr, tf->elr, tf->spsr, far);

    switch (ec) {
    case 0x15:  // SVC64: tf->elr already points past the svc, so resume as-is.
        kprintf("[M3b] svc EC=0x%x handled (resume)\n", ec);
        break;
    case 0x3c:  // BRK64: tf->elr points AT the brk, advance to skip.
        kprintf("[M3c] brk EC=0x%x handled (skip)\n", ec);
        tf->elr += 4;
        break;
    case 0x24:  // data abort, lower EL
    case 0x25:  // data abort, current EL (M4 unmapped-access demo). FAR = bad VA.
        kprintf("[M4b] data abort EC=0x%x FAR=0x%lx handled (skip)\n", ec, far);
        tf->elr += 4;
        break;
    default:
        kprintf("  UNEXPECTED exception — halting (watchdog will reap)\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    return tf;
}

void vectors_init(void)
{
    extern char exc_vectors[];
    SYSREG_WRITE("vbar_el1", (uintptr_t)exc_vectors);
    __asm__ volatile("isb");
}
