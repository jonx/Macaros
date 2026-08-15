// AROS AArch64 bring-up — the exception vector table (M3) + full-context save (M10).
//
// 16 entries × 0x80 bytes (2 KiB total), VBAR_EL1 2 KiB-aligned. Four groups of
// four (Sync, IRQ, FIQ, SError); we run at EL1h so traps land in the "Current EL
// with SP_ELx" group at offset 0x200.
//
// Each entry branches to a trampoline that saves a FULL trap frame (x0-x30 + ELR
// + SPSR — the ELR/SPSR are needed so a preemptive switch can resume a *different*
// task), calls the C handler, then restores from the frame the handler RETURNS
// (same frame for a normal return; another task's frame for a preemptive switch).
//
// struct trapframe (must match kern.h): x[0..30] at 0..240, elr at 256, spsr 264.

.macro VEC target
    .balign 0x80
    b   \target
.endm

.macro EXC_SAVE
    sub  sp, sp, #272
    stp  x0,  x1,  [sp, #16*0]
    stp  x2,  x3,  [sp, #16*1]
    stp  x4,  x5,  [sp, #16*2]
    stp  x6,  x7,  [sp, #16*3]
    stp  x8,  x9,  [sp, #16*4]
    stp  x10, x11, [sp, #16*5]
    stp  x12, x13, [sp, #16*6]
    stp  x14, x15, [sp, #16*7]
    stp  x16, x17, [sp, #16*8]
    stp  x18, x19, [sp, #16*9]
    stp  x20, x21, [sp, #16*10]
    stp  x22, x23, [sp, #16*11]
    stp  x24, x25, [sp, #16*12]
    stp  x26, x27, [sp, #16*13]
    stp  x28, x29, [sp, #16*14]
    str  x30,      [sp, #240]
    mrs  x0, elr_el1            // x0/x1 already saved above, reuse as scratch
    mrs  x1, spsr_el1
    stp  x0, x1,   [sp, #256]
.endm

.macro EXC_RESTORE
    ldp  x0, x1,   [sp, #256]   // ELR, SPSR first (uses x0/x1 as scratch)
    msr  elr_el1,  x0
    msr  spsr_el1, x1
    ldp  x0,  x1,  [sp, #16*0]  // then the GPRs (overwrites the scratch)
    ldp  x2,  x3,  [sp, #16*1]
    ldp  x4,  x5,  [sp, #16*2]
    ldp  x6,  x7,  [sp, #16*3]
    ldp  x8,  x9,  [sp, #16*4]
    ldp  x10, x11, [sp, #16*5]
    ldp  x12, x13, [sp, #16*6]
    ldp  x14, x15, [sp, #16*7]
    ldp  x16, x17, [sp, #16*8]
    ldp  x18, x19, [sp, #16*9]
    ldp  x20, x21, [sp, #16*10]
    ldp  x22, x23, [sp, #16*11]
    ldp  x24, x25, [sp, #16*12]
    ldp  x26, x27, [sp, #16*13]
    ldp  x28, x29, [sp, #16*14]
    ldr  x30,      [sp, #240]
    add  sp, sp, #272
.endm

.macro EXC_TRAMP kind
    EXC_SAVE
    mov  x1, #\kind            // kind: 0 sync, 1 irq, 2 fiq, 3 serror, 4 invalid
    mov  x0, sp                // x0 = current trap frame
    bl   exc_handler           // returns the frame to restore (may be another task)
    mov  sp, x0                // switch to it
    EXC_RESTORE
    eret
.endm

.section .text
.balign 0x800
.global exc_vectors
exc_vectors:
    VEC vec_invalid     // Cur EL SP0:    Sync
    VEC vec_invalid     //                IRQ
    VEC vec_invalid     //                FIQ
    VEC vec_invalid     //                SError
    VEC vec_el1_sync    // Cur EL SPx:    Sync   (0x200)
    VEC vec_el1_irq     //                IRQ    (0x280)
    VEC vec_el1_fiq     //                FIQ
    VEC vec_el1_serr    //                SError
    VEC vec_invalid     // Lower AArch64: Sync
    VEC vec_invalid     //                IRQ
    VEC vec_invalid     //                FIQ
    VEC vec_invalid     //                SError
    VEC vec_invalid     // Lower AArch32: Sync
    VEC vec_invalid     //                IRQ
    VEC vec_invalid     //                FIQ
    VEC vec_invalid     //                SError

vec_el1_sync: EXC_TRAMP 0
vec_el1_irq:  EXC_TRAMP 1
vec_el1_fiq:  EXC_TRAMP 2
vec_el1_serr: EXC_TRAMP 3
vec_invalid:  EXC_TRAMP 4
