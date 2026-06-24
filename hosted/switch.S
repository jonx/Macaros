// Hosted AArch64 AROS — cooperative context switch for macOS (Mach-O).
//
// Identical coroutine switch to the bare-metal boot/switch.S, but for Mach-O the
// symbol carries a leading underscore (_ctx_switch). It runs at EL0 inside a
// normal macOS process — ctx_switch uses no privileged instructions, so the same
// register save/restore that drove the native scheduler works hosted too. That
// portability is the first thing the spike proves.
//
// struct context offsets (must match host.c):
//   x19=0 x20=8 x21=16 x22=24 x23=32 x24=40 x25=48 x26=56
//   x27=64 x28=72 x29=80 x30=88 sp=96

.text
.globl _ctx_switch
.p2align 2
// void ctx_switch(struct context *old /*x0*/, struct context *next /*x1*/)
_ctx_switch:
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
    ret
