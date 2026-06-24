// Hosted AArch64 AROS — Phase 2 H3: the host-call ABI shim (hand-written).
//
// This is the make-or-break layer of a hosted port — the boundary that killed
// the old Darwin-PPC effort. Real AROS code is built by AROS's own crosstools to
// the generic AAPCS64 ABI; when it calls a macOS libc function it crosses into
// APPLE's arm64 ABI, which diverges. The one that bites is variadic calls:
//
//   Generic AAPCS64:  variadic args fill x0..x7 first, then spill to the stack.
//   Apple arm64:      fixed args go in registers; ALL variadic args go on the
//                     STACK, one 8-byte slot each — even when x-registers are free.
//
// Grounded against the assembly Apple clang emits for snprintf("%d %d %d",...):
// the three ints are `str`'d to [sp], [sp+8], [sp+16] while x5..x7 sit unused
// (see NOTES.md "H3 grounding"). A hosted AROS thus cannot just `bl _printf`
// with args in registers; it must marshal the variadic tail onto the stack. This
// file is that marshaller, written by hand the way AROS's host bridge must.
//
// int aros_call_snprintf(char *buf, unsigned long size, const char *fmt,
//                        long argc, const unsigned long *argv);
//   x0=buf  x1=size  x2=fmt  x3=argc  x4=argv
// buf/size/fmt are snprintf's three FIXED args — they pass straight through in
// x0/x1/x2. argv[0..argc) is the variadic tail, marshalled onto the stack.

.text
.globl _aros_call_snprintf
.p2align 2
_aros_call_snprintf:
    stp  x29, x30, [sp, #-16]!      // open a frame (keeps sp 16-byte aligned)
    mov  x29, sp

    // bytes = round_up(argc*8, 16) — the variadic area, kept 16-byte aligned.
    lsl  x9, x3, #3                 // x9 = argc * 8
    add  x9, x9, #15
    and  x9, x9, #0xfffffffffffffff0
    sub  sp, sp, x9                 // reserve the stack varargs area

    // Copy argv[i] -> [sp + i*8]. Each variadic arg is one 8-byte slot; a host
    // double rides through as its bit pattern (Apple puts variadic FP on the
    // stack too), so this one integer path carries %d, %p and %f alike.
    mov  x10, #0                    // byte offset
    mov  x11, #0                    // index
1:
    cmp  x11, x3
    b.ge 2f
    ldr  x12, [x4, x11, lsl #3]     // x12 = argv[x11]
    str  x12, [sp, x10]
    add  x10, x10, #8
    add  x11, x11, #1
    b    1b
2:
    bl   _snprintf                  // x0/x1/x2 still buf/size/fmt; varargs at [sp..]
    mov  sp, x29                    // drop the varargs area
    ldp  x29, x30, [sp], #16
    ret

// Negative control — the NAIVE generic-AAPCS64 path: pass the variadic tail in
// registers (x3,x4,x5 after the three fixed args) the way a port that ignored
// Apple's divergence would. Apple's snprintf reads varargs from the STACK, so it
// sees the zeroed slots here, not our registers -> deterministically wrong. This
// is exactly the bug the grounding was meant to prevent. Fixed 3-arg arity; it
// exists only to make the divergence observable in the loop.
//   int aros_call_snprintf_aapcs(buf, size, fmt, argc(=3), argv)
.globl _aros_call_snprintf_aapcs
.p2align 2
_aros_call_snprintf_aapcs:
    stp  x29, x30, [sp, #-16]!
    mov  x29, sp
    sub  sp, sp, #32                // a zeroed stack varargs area (>= 3 slots)
    stp  xzr, xzr, [sp]
    stp  xzr, xzr, [sp, #16]
    ldr  x3, [x4]                   // argv[0] -> x3   (read argv before clobbering x4)
    ldr  x5, [x4, #16]              // argv[2] -> x5
    ldr  x4, [x4, #8]              // argv[1] -> x4   (clobber x4 last)
    bl   _snprintf                  // Apple reads the (zeroed) stack, not x3..x5
    mov  sp, x29
    ldp  x29, x30, [sp], #16
    ret
