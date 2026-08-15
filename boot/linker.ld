/* AROS AArch64 bring-up — link the M1 stub for QEMU 'virt'.
 *
 * RAM on 'virt' begins at 0x4000_0000. QEMU's -kernel loader places an ELF at
 * the addresses in its program headers and jumps to ENTRY. 0x4008_0000 is the
 * conventional kernel load offset (0x40000000 + 0x80000), leaving the bottom of
 * RAM free for QEMU's boot shim and the DTB.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : { KEEP(*(.text.boot)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    . = ALIGN(8);
    .bss : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* 16 KiB stack reserved above everything; SP starts at _stack_top and
     * grows down into this gap, so it can never collide with .bss. */
    . = ALIGN(16);
    . += 0x4000;
    _stack_top = .;

    . = ALIGN(0x1000);
    _end = .;               /* first page past the image; pmm heap starts here */
}
