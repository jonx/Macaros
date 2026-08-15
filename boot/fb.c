// AROS AArch64 bring-up — M9: a framebuffer via ramfb (the pixel "way of seeing").
//
// QEMU 'virt' has no default display, so we use ramfb: a RAM framebuffer the guest
// sets up by writing a config blob to the "etc/ramfb" fw_cfg file via the fw_cfg
// DMA interface. We draw four colored quadrants and let QEMU scan them out; the
// harness captures them with a QMP screendump.
//
// Grounded from QEMU qemu_fw_cfg.h (DMA control bits, FILE_DIR=0x19, MMIO is
// big-endian) and hw/display/ramfb.c (RAMFBCfg = {u64 addr; u32 fourcc, flags,
// width, height, stride}, all big-endian). fw_cfg MMIO base 0x0902_0000 (virt).

#include "kern.h"

#define FWCFG_BASE      0x09020000UL
#define FWCFG_DMA       0x10            // 64-bit DMA register (write BE ctl-struct addr)

#define FW_CFG_FILE_DIR 0x19
#define DMA_CTL_ERROR   0x01
#define DMA_CTL_READ    0x02
#define DMA_CTL_SELECT  0x08
#define DMA_CTL_WRITE   0x10

#define FB_W 640
#define FB_H 480

static uint32_t framebuffer[FB_W * FB_H] __attribute__((aligned(4096)));

struct fw_dma  { uint32_t control; uint32_t length; uint64_t address; } __attribute__((packed));
struct ramfb_cfg {
    uint64_t addr; uint32_t fourcc; uint32_t flags;
    uint32_t width; uint32_t height; uint32_t stride;
} __attribute__((packed));

static inline uint32_t bs32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t bs64(uint64_t x) { return __builtin_bswap64(x); }

// One fw_cfg DMA transfer (read or write) of an item by key.
static void fw_cfg_dma(uint32_t key, void *buf, uint32_t len, int write)
{
    static volatile struct fw_dma dma __attribute__((aligned(16)));
    dma.control = bs32((key << 16) | DMA_CTL_SELECT | (write ? DMA_CTL_WRITE : DMA_CTL_READ));
    dma.length  = bs32(len);
    dma.address = bs64((uint64_t)(uintptr_t)buf);
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint64_t *)(FWCFG_BASE + FWCFG_DMA) = bs64((uint64_t)(uintptr_t)&dma);
    __asm__ volatile("dsb sy" ::: "memory");
    while (bs32(dma.control) & ~DMA_CTL_ERROR) { }       // spin until QEMU finishes
}

// Scan the fw_cfg file directory for `name`; return its selector key or -1.
static int fw_cfg_find(const char *name)
{
    static uint8_t dir[8192];
    fw_cfg_dma(FW_CFG_FILE_DIR, dir, sizeof(dir), 0);
    uint32_t count = bs32(*(uint32_t *)dir);
    uint8_t *p = dir + 4;                                // entries: u32 size,u16 sel,u16 rsvd,char[56]
    for (uint32_t i = 0; i < count && (p + 64) <= dir + sizeof(dir); i++, p += 64) {
        uint16_t sel = (uint16_t)((p[4] << 8) | p[5]);   // big-endian u16
        const char *fname = (const char *)(p + 8);
        int eq = 1;
        for (int k = 0; ; k++) {
            if (name[k] != fname[k]) { eq = 0; break; }
            if (name[k] == 0) break;
        }
        if (eq) return sel;
    }
    return -1;
}

static void draw_quadrants(void)
{
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++) {
            uint32_t c;
            if (y < FB_H / 2) c = (x < FB_W / 2) ? 0x00FF0000 : 0x0000FF00;  // red  | green
            else              c = (x < FB_W / 2) ? 0x000000FF : 0x00FFFFFF;  // blue | white
            framebuffer[y * FB_W + x] = c;
        }
}

void fb_init(void)
{
    draw_quadrants();
    int key = fw_cfg_find("etc/ramfb");
    if (key < 0) {
        kprintf("[M9] ERROR: etc/ramfb not found in fw_cfg\n");
        return;
    }
    struct ramfb_cfg cfg = {
        .addr   = bs64((uint64_t)(uintptr_t)framebuffer),
        .fourcc = bs32(0x34325258),     // 'XR24' = DRM_FORMAT_XRGB8888
        .flags  = 0,
        .width  = bs32(FB_W),
        .height = bs32(FB_H),
        .stride = bs32(FB_W * 4),
    };
    fw_cfg_dma((uint32_t)key, &cfg, sizeof(cfg), 1);
    kprintf("[M9] framebuffer up: %dx%d XRGB8888 @ %p (ramfb key=%d, 4 quadrants)\n",
            FB_W, FB_H, framebuffer, key);
}
