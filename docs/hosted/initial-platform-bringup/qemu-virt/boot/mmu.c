// AROS AArch64 bring-up — M4: turn on the MMU without vanishing.
//
// Foreshadows AROS's `arch/<cpu>-native/kernel/mmu.c` (core_SetupMMU). We build
// the simplest possible identity map: one level-1 table (4KB granule, T0SZ=25 →
// 39-bit VA → start at L1), 512 entries of 1GB blocks. Identity means VA==PA, so
// the PC/SP/UART addresses don't move when the MMU comes on and execution
// continues seamlessly.
//
// Descriptor bits grounded from Linux arch/arm64 pgtable-hwdef.h:
//   block=0b01[1:0], AttrIndx[4:2], AP[1]=bit6, SH[9:8], AF=bit10, PXN=53, UXN=54.

#include "kern.h"

// MAIR attribute indices we define.
#define MT_DEVICE   0      // Device-nGnRnE (MAIR Attr0 = 0x00)
#define MT_NORMAL   1      // Normal write-back  (MAIR Attr1 = 0xFF)

// Block-descriptor bits.
#define DESC_BLOCK   (1UL << 0)
#define DESC_AIDX(i) ((uint64_t)(i) << 2)
#define DESC_SH_IS   (3UL << 8)     // inner shareable
#define DESC_AF      (1UL << 10)    // access flag (or first touch faults)
#define DESC_PXN     (1UL << 53)
#define DESC_UXN     (1UL << 54)

// One L1 table: 512 × 1GB = 512GB of VA. 4KB-aligned, lives in zeroed .bss.
static uint64_t l1_table[512] __attribute__((aligned(4096)));

void mmu_init(void)
{
    // Low 1GB = devices (UART 0x0900_0000, GIC 0x0800_0000): Device, no execute.
    l1_table[0] = 0x00000000UL | DESC_BLOCK | DESC_AIDX(MT_DEVICE)
                | DESC_AF | DESC_PXN | DESC_UXN;
    // 0x4000_0000..0x8000_0000 = RAM (code, stack, page table): Normal, cacheable,
    // inner-shareable, kernel-executable (PXN clear), EL0 no-exec (UXN set).
    l1_table[1] = 0x40000000UL | DESC_BLOCK | DESC_AIDX(MT_NORMAL)
                | DESC_AF | DESC_SH_IS | DESC_UXN;
    // entries 2..511 stay 0 (invalid) — we touch nothing above 2GB yet.

    uint64_t mair = (0x00UL << (8 * MT_DEVICE))    // Attr0: Device-nGnRnE
                  | (0xFFUL << (8 * MT_NORMAL));    // Attr1: Normal WB RW-allocate

    uint64_t tcr = (25UL << 0)     // T0SZ = 25 (39-bit VA, start at L1)
                 | (1UL  << 8)     // IRGN0 = WB cacheable
                 | (1UL  << 10)    // ORGN0 = WB cacheable
                 | (3UL  << 12)    // SH0   = inner shareable
                 | (0UL  << 14)    // TG0   = 4KB granule
                 | (1UL  << 23)    // EPD1  = disable TTBR1 walks (we use TTBR0 only)
                 | (1UL  << 32);   // IPS   = 36-bit PA (headroom over our <2GB)

    SYSREG_WRITE("mair_el1", mair);
    SYSREG_WRITE("tcr_el1", tcr);
    SYSREG_WRITE("ttbr0_el1", (uintptr_t)l1_table);
    __asm__ volatile("dsb ish; isb");                 // table writes visible first
    __asm__ volatile("tlbi vmalle1; dsb ish; isb");   // flush stale translations

    // Flip the switch: M (MMU) + C (D-cache) + I (I-cache), preserving RES1 bits.
    uint64_t sctlr = SYSREG_READ("sctlr_el1");
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    SYSREG_WRITE("sctlr_el1", sctlr);
    __asm__ volatile("isb");                          // next fetch is translated
}
