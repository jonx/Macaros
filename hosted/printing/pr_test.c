/* pr_test.c — [PRPDF] headless print-to-PDF proof: generate PDFs with the host
 * engine, then REOPEN them with CGPDFDocument and assert structure (no human, no
 * print dialog, no TCC). Grounds the PDF-direct engine the printing bridge's
 * [PR4] driver path uses.
 *
 * Independent work; resemblance coincidental. Mirrors the project's "a file
 * existing is not a PASS — its contents are" discipline (hosted/display.c).
 *
 *   [PRPDF1] a 2-page document (CoreText text page + RGBA raster page) parses as a
 *            valid PDF with exactly 2 pages and the %PDF- magic.
 *   [PRPDF2] the one-shot pdf_raster_to_pdf (== spec cups_raster_to_pdf) yields a
 *            valid 1-page PDF.
 *
 * Prints "[PRPDF] PASS"; exits 0 on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include "pdfgen.h"

static int verify_pdf(const char *path, int expectPages)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  verify: cannot open %s\n", path); return -1; }
    char hdr[5] = {0};
    size_t n = fread(hdr, 1, 5, f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (n < 5 || memcmp(hdr, "%PDF-", 5) != 0) { printf("  verify: bad magic\n"); return -1; }

    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, false);
    CFRelease(s);
    CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
    CFRelease(url);
    if (!doc) { printf("  verify: CGPDFDocument parse failed\n"); return -1; }
    int pages = (int)CGPDFDocumentGetNumberOfPages(doc);
    CGPDFDocumentRelease(doc);
    if (pages != expectPages) { printf("  verify: pages %d != %d\n", pages, expectPages); return -1; }

    printf("  verify: %%PDF- magic, pages=%d, size=%ldB\n", pages, sz);
    return 0;
}

int main(void)
{
    mkdir("run", 0755);
    int ok = 1;

    /* A known RGBA raster (the printer.device graphics-dump path stands in here). */
    int w = 300, h = 200;
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) { printf("[PRPDF] FAIL oom\n"); return 1; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 128; p[3] = 255;
        }

    /* [PRPDF1] multi-page: real text + raster, the way a printed document arrives. */
    PDFDoc *d = pdf_begin("run/pr_multi.pdf");
    if (!d) { printf("[PRPDF1] FAIL pdf_begin\n"); ok = 0; }
    else {
        pdf_add_text_page(d,
            "AROS print-to-PDF prep (hosted, aarch64-darwin)\n\n"
            "This page is REAL, selectable text laid out via CoreText -- the path a\n"
            "printer.device text stream takes. The next page is a rasterised graphics\n"
            "dump. macOS owns the PDF; AROS reaches it via standard exec I/O.\n", 14.0);
        pdf_add_raster_page(d, rgba, w, h);
        pdf_finish(d);
        printf("[PRPDF1] wrote run/pr_multi.pdf (text + raster)\n");
        if (verify_pdf("run/pr_multi.pdf", 2) != 0) ok = 0;
        else printf("[PRPDF1] PASS\n");
    }

    /* [PRPDF2] one-shot raster (== spec cups_raster_to_pdf). */
    if (pdf_raster_to_pdf("run/pr_raster.pdf", rgba, w, h) != 0) { printf("[PRPDF2] FAIL raster_to_pdf\n"); ok = 0; }
    else {
        printf("[PRPDF2] wrote run/pr_raster.pdf (one-shot raster)\n");
        if (verify_pdf("run/pr_raster.pdf", 1) != 0) ok = 0;
        else printf("[PRPDF2] PASS\n");
    }

    free(rgba);

    if (ok) { printf("[PRPDF] PASS print-to-PDF host engine verified\n"); return 0; }
    printf("[PRPDF] FAIL\n");
    return 1;
}
