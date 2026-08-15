// AROS AArch64 bring-up — M7: the cooperative context switch.
//
// Foreshadows AROS's cpu_Switch/cpu_Dispatch. This is a coroutine-style switch:
// it saves only the AAPCS callee-saved state (x19-x28, FP=x29, LR=x30, SP) of the
// current task into *old and loads *next, then `ret`s into the new task's LR.
// Caller-saved registers are the C caller's responsibility, so they need no
// saving here. (A *preemptive* switch from an IRQ would instead save the full
// trap frame — that's a later concern; vectors.S already does it for handlers.)
//
// struct context offsets (must match kern.h):
//   x19=0 x20=8 x21=16 x22=24 x23=32 x24=40 x25=48 x26=56
//   x27=64 x28=72 x29=80 x30=88 sp=96

.global ctx_switch
// void ctx_switch(struct context *old /*x0*/, struct context *next /*x1*/)
ctx_switch:
    stp  x19, x20, [x0, #0]
    stp  x21, x22, [x0, #16]
    stp  x23, x24, [x0, #32]
    stp  x25, x26, [x0, #48]
    stp  x27, x28, [x0, #64]
    stp  x29, x30, [x0, #80]
    mov  x2, sp
    str  x2, [x0, #96]

    ldp  x19, x20, [x1, #0]
    ldp  x21, x22, [x1, #16]
    ldp  x23, x24, [x1, #32]
    ldp  x25, x26, [x1, #48]
    ldp  x27, x28, [x1, #64]
    ldp  x29, x30, [x1, #80]
    ldr  x2, [x1, #96]
    mov  sp, x2
    ret                         // returns into next->x30 (fresh entry or resume point)
