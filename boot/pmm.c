// AROS AArch64 bring-up — M6: a physical page allocator.
//
// Foreshadows the substrate AROS's kernel_mm.c / exec memory pools sit on. A
// dead-simple free-list of 4KB pages: each free page stores the next pointer in
// its first word. Heap = [page-aligned _end .. PHYS_RAM_END).
//
// Grounded note: PHYS_RAM_END is tied to the harness's pinned `-m 512`
// (RAM 0x4000_0000 + 512 MiB). The *proper* source is the device tree's
// memory node — deferred until we parse a DTB (x0 doesn't give it to us; see
// HARDWARE.md). Until then this is an explicit, documented assumption.

#include "kern.h"

#define PHYS_RAM_BASE 0x40000000UL
#define PHYS_RAM_END  (PHYS_RAM_BASE + 512UL * 1024 * 1024)   // matches -m 512
#define PAGE_SIZE     4096UL

extern char _end[];                         // first byte past the kernel image

static void *free_list = 0;                 // LIFO stack of free pages
static uint64_t free_pages = 0;

uint64_t pmm_free_count(void) { return free_pages; }

void pmm_init(void)
{
    uintptr_t start = ((uintptr_t)_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uintptr_t p = start; p + PAGE_SIZE <= PHYS_RAM_END; p += PAGE_SIZE) {
        *(void **)p = free_list;
        free_list = (void *)p;
        free_pages++;
    }
}

void *pmm_alloc(void)
{
    if (!free_list)
        return 0;
    void *p = free_list;
    free_list = *(void **)p;
    free_pages--;
    return p;
}

void pmm_free(void *p)
{
    *(void **)p = free_list;
    free_list = p;
    free_pages++;
}
