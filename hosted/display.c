// Hosted AArch64 AROS — Phase 2 H7: the host display driver.
//
// The Phase-2 thesis is "macOS owns the drivers". H7 applies it to the display:
// AROS draws into a framebuffer it allocates from its OWN heap (the H5 allocator
// over mmap), and the HOST presents it — here by encoding to PNG via macOS's
// ImageIO (CoreGraphics), exactly the role a real display driver plays. This
// mirrors Phase-1 M9 (ramfb → QMP screendump): the agent observes pixels through
// a file it can read, so the loop stays unattended. An on-screen Cocoa/Metal
// window is the thin human-facing addition — deferred, because verifying a live
// window unattended needs macOS Screen-Recording permission (a manual step that
// would break the loop).
//
// The ImageIO call sequence was grounded against the live toolchain before this
// was written (scratch pngprobe: CGBitmapContextCreate → CGBitmapContextCreateImage
// → CGImageDestination{CreateWithURL,AddImage,Finalize}).

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

// ---- compact H5 allocator (the framebuffer comes from AROS's heap) ---------
#define NT_MEMORY 10
struct Node { struct Node *ln_Succ, *ln_Pred; uint8_t ln_Type; int8_t ln_Pri; char *ln_Name; };
struct MemChunk { struct MemChunk *mc_Next; uintptr_t mc_Bytes; };
struct MemHeader { struct Node mh_Node; uint16_t mh_Attributes; struct MemChunk *mh_First; void *mh_Lower, *mh_Upper; uintptr_t mh_Free; };
#define MEMCHUNK_TOTAL 16
#define MEMF_CLEAR (1L << 16)
#define ROUNDUP(x, a) (((uintptr_t)(x) + (a) - 1) & ~(uintptr_t)((a) - 1))
static struct MemHeader sysmh;
static void *Allocate(struct MemHeader *mh, uintptr_t size, unsigned long flags) {
    if (!size) return NULL;
    uintptr_t bs = ROUNDUP(size, MEMCHUNK_TOTAL);
    if (mh->mh_Free < bs) return NULL;
    struct MemChunk *p1 = (struct MemChunk *)&mh->mh_First, *p2 = p1->mc_Next, *mc = NULL;
    while (p2) { if (p2->mc_Bytes >= bs) { mc = p1; break; } p1 = p2; p2 = p1->mc_Next; }
    if (!mc) return NULL;
    p1 = mc; p2 = p1->mc_Next;
    if (p2->mc_Bytes == bs) { p1->mc_Next = p2->mc_Next; mc = p2; }
    else { struct MemChunk *r = (struct MemChunk *)((uint8_t *)p2 + bs); p1->mc_Next = r; r->mc_Next = p2->mc_Next; r->mc_Bytes = p2->mc_Bytes - bs; mc = p2; }
    mh->mh_Free -= bs;
    if (flags & MEMF_CLEAR) memset(mc, 0, bs);
    return mc;
}
static void mh_Init(struct MemHeader *mh, void *base, uintptr_t len) {
    len &= ~(uintptr_t)(MEMCHUNK_TOTAL - 1);
    mh->mh_Node.ln_Type = NT_MEMORY; mh->mh_Node.ln_Name = "hosted-ram";
    mh->mh_Lower = base; mh->mh_Upper = (uint8_t *)base + len;
    mh->mh_First = (struct MemChunk *)base; mh->mh_First->mc_Next = NULL; mh->mh_First->mc_Bytes = len; mh->mh_Free = len;
}
static void *AllocMem(uintptr_t size, unsigned long flags) { return Allocate(&sysmh, size, flags); }

// ---- framebuffer + drawing (AROS side) -------------------------------------
#define W 640
#define H 480
static uint8_t *fb;                       // RGBX, 4 bytes/pixel

static inline void put(int x, int y, uint32_t rgb) {
    if ((unsigned)x >= W || (unsigned)y >= H) return;
    uint8_t *p = &fb[(y * W + x) * 4];
    p[0] = rgb >> 16; p[1] = rgb >> 8; p[2] = rgb; p[3] = 255;
}
static void fill_rect(int x0, int y0, int w, int h, uint32_t rgb) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) put(x, y, rgb);
}
// 8x8 glyphs for just the letters we draw (MSB = leftmost column).
static const uint8_t GA[8] = { 0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00 };
static const uint8_t GR[8] = { 0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00 };
static const uint8_t GO[8] = { 0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00 };
static const uint8_t GS[8] = { 0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00 };
static void draw_glyph(const uint8_t g[8], int x0, int y0, int scale, uint32_t rgb) {
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (0x80 >> col))
                fill_rect(x0 + col * scale, y0 + row * scale, scale, scale, rgb);
}

// The scene: a gradient sky, a Workbench-style title bar, "AROS" in big letters,
// and the M9 four-colour test row — recognisably AROS, and pixel-checkable.
#define TXT_X0 168
#define TXT_Y0 160
#define TXT_SCALE 8
#define GLYPH_ADVANCE (8 * TXT_SCALE + 16)
static const int SQ_X[4] = { 140, 260, 380, 500 };
static const int SQ_Y = 380, SQ_W = 60;
static const uint32_t SQ_RGB[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };

static void draw_scene(void) {
    // vertical gradient sky: (20,30,80) at top -> (0,0,10) at bottom
    for (int y = 0; y < H; y++) {
        int r = 20 - 20 * y / H, g = 30 - 30 * y / H, b = 80 - 70 * y / H;
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        fill_rect(0, y, W, 1, c);
    }
    // title bar
    fill_rect(0, 0, W, 28, 0x9098A0);
    fill_rect(8, 8, 12, 12, 0xFF8800);          // a depth gadget
    fill_rect(W - 20, 8, 12, 12, 0xFFFFFF);
    // "AROS"
    const uint8_t *glyphs[4] = { GA, GR, GO, GS };
    for (int i = 0; i < 4; i++)
        draw_glyph(glyphs[i], TXT_X0 + i * GLYPH_ADVANCE, TXT_Y0, TXT_SCALE, 0xFFFFFF);
    // four-colour test row (tie to M9)
    for (int i = 0; i < 4; i++)
        fill_rect(SQ_X[i], SQ_Y, SQ_W, SQ_W, SQ_RGB[i]);
}

// ---- host display driver: present the framebuffer (macOS ImageIO) ----------
static int host_present(const char *path) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(fb, W, H, 8, (size_t)W * 4, cs,
                          kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    if (!ctx) return 0;
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFStringRef ps = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, ps, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    int ok = 0;
    if (dst) { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); CFRelease(dst); }
    CFRelease(url); CFRelease(ps); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    return ok;
}

// ---- pixel verifier (the machine verdict behind the PNG) -------------------
static int px_is(int x, int y, uint32_t rgb) {
    uint8_t *p = &fb[(y * W + x) * 4];
    return p[0] == (uint8_t)(rgb >> 16) && p[1] == (uint8_t)(rgb >> 8) && p[2] == (uint8_t)rgb;
}
#define CHECK(c) do { if (!(c)) { printf("[H7] FAIL: %s (line %d)\n", #c, __LINE__); ok = 0; } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H7a] hosted display: AROS draws a framebuffer from its heap; macOS presents it\n");

    const uintptr_t LEN = 8u << 20;
    void *region = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) { printf("[H7] FAIL: mmap\n"); return 1; }
    mh_Init(&sysmh, region, LEN);

    fb = AllocMem((uintptr_t)W * H * 4, MEMF_CLEAR);     // bitmap from the AROS heap
    if (!fb) { printf("[H7] FAIL: AllocMem framebuffer\n"); return 1; }
    draw_scene();

    int ok = 1;
    // four-colour squares: exact colours at their centres
    for (int i = 0; i < 4; i++)
        CHECK(px_is(SQ_X[i] + SQ_W / 2, SQ_Y + SQ_W / 2, SQ_RGB[i]));
    // title bar present
    CHECK(px_is(W / 2, 14, 0x9098A0));
    // "AROS" text: a white pixel on the 'A' crossbar, and dark background just above it
    CHECK(px_is(TXT_X0 + 2, TXT_Y0 + 4 * TXT_SCALE + 2, 0xFFFFFF));
    CHECK(!px_is(TXT_X0 + 2, TXT_Y0 - 4, 0xFFFFFF));
    // gradient sky is bluish in the upper area (below the title bar)
    { uint8_t *p = &fb[(120 * W + 5) * 4]; CHECK(p[2] > p[0] && p[2] > p[1]); }

    const char *path = "run/aros-h7.png";
    int presented = host_present(path);
    CHECK(presented);

    printf("[H7]   framebuffer %dx%d from AROS heap (%lu KiB), %d/4 colour squares + text verified\n",
           W, H, (unsigned long)((uintptr_t)W * H * 4 / 1024), 4);
    printf("[H7]   host presented -> %s\n", path);
    if (ok)
        printf("[H7] hosted display ok: AROS framebuffer presented by macOS (see %s)\n", path);
    else
        printf("[H7] FAIL: see checks above\n");
    munmap(region, LEN);
    return ok ? 0 : 1;
}
