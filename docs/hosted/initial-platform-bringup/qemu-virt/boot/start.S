// AROS AArch64 bring-up — the bare-metal entry stub.
//
// Job: get the CPU into a state where C can run, then hand off to kmain().
//   1. park secondary cores (only the boot CPU proceeds)
//   2. set up the stack
//   3. zero .bss (C assumes it)
//   4. call kmain() in C
//   5. on return, exit QEMU cleanly via semihosting (PASS in <1s)
//
// QEMU 'virt' enters the primary CPU here at EL1 with -kernel (confirmed via the
// -d int trace at M1). Everything device-specific lives in C from M2 onward.

.section .text.boot, "ax"
.global _start
_start:
    // Entry state (verified): MMU off, all interrupts masked in DAIF, EL1.
    // NOTE: the arm64 Linux boot protocol says x0 = DTB phys, but that applies to
    // the Image-header path. QEMU's ELF `-kernel` path enters with x0 = 0 (proven
    // at M2 — it prints 0x0). Stash x0 anyway so the ABI is right if we later boot
    // via a path that does pass a DTB; M6 obtains the device tree explicitly.
    mov     x19, x0

    // Park secondary cores: only the boot CPU (affinity 0) proceeds. (On QEMU
    // virt, PSCI already holds secondaries off, so this is belt-and-suspenders.)
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, halt

    // Stack (reserved at the top of the image by the linker script).
    adrp    x0, _stack_top
    add     x0, x0, :lo12:_stack_top
    mov     sp, x0

    // Zero .bss so C statics start at 0. Bounds are 8-byte aligned (linker.ld).
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:
    cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    mov     x0, x19                     // pass DTB phys to kmain(dtb)
    bl      kmain

exit:
    // Semihosting SYS_EXIT (requires -semihosting). On AArch64 the parameter
    // register points to a {reason, exit_code} block — NOT the reason directly.
    mov     x0, #0x18                   // SYS_EXIT
    adrp    x1, exit_block
    add     x1, x1, :lo12:exit_block
    hlt     #0xf000

halt:
    // Fallback if semihosting is disabled: spin so the harness timeout reaps us.
    wfe
    b       halt

.section .data
.align 3
exit_block:
    .quad   0x20026                     // ADP_Stopped_ApplicationExit
    .quad   0                           // exit status 0
