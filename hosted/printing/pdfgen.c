/* pdfgen.c — host PDF-generation engine for the printing bridge (print-to-PDF).
 *
 * Independent work: no third-party implementation source was read, searched, or
 * consulted; any resemblance is coincidental. Implemented from
 * docs/features/printing/spec.md against Apple's documented CoreGraphics
 * (CGPDFContext) and CoreText interfaces [PUB]. Generates PDF on our side — never
 * the macOS-14-removed PostScript→PDF path. Read-only of the host except the output
 * file; no entitlement, no TCC, no print dialog.
 *
 * Pulls NO AROS headers. Plain C over CoreFoundation/CoreGraphics/CoreText; links
 * into libpdfgen.dylib (dlopen'd by the AROS side via hostlib.resource) and can be
 * compiled straight into the Daedalos app.
 */
#include "pdfgen.h"

#include <stdlib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

struct PDFDoc { CGContextRef ctx; double w, h; };

/* US Letter in points. */
static CGRect letter_media(void) { return CGRectMake(0, 0, 612.0, 792.0); }

PDFDoc *pdf_begin(const char *path)
{
    if (!path) return NULL;
    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    if (!s) return NULL;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, false);
    CFRelease(s);
    if (!url) return NULL;

    CGRect media = letter_media();
    CGContextRef ctx = CGPDFContextCreateWithURL(url, &media, NULL);
    CFRelease(url);
    if (!ctx) return NULL;

    PDFDoc *d = (PDFDoc *)calloc(1, sizeof *d);
    if (!d) { CGContextRelease(ctx); return NULL; }
    d->ctx = ctx;
    d->w = media.size.width;
    d->h = media.size.height;
    return d;
}

int pdf_add_raster_page(PDFDoc *d, const void *rgba, int w, int h)
{
    if (!d || !rgba || w <= 0 || h <= 0) return -1;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, rgba,
                                                        (size_t)w * (size_t)h * 4, NULL);
    if (!cs || !dp) { if (dp) CGDataProviderRelease(dp); if (cs) CGColorSpaceRelease(cs); return -1; }

    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault,
                                   dp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (!img) return -1;

    CGRect media = CGRectMake(0, 0, d->w, d->h);
    CGContextBeginPage(d->ctx, &media);

    /* Fit the raster into the page, aspect-preserved, centred, with a margin. */
    const double margin = 36.0;
    double availW = d->w - 2 * margin, availH = d->h - 2 * margin;
    double sx = availW / (double)w, sy = availH / (double)h;
    double sc = sx < sy ? sx : sy;
    double dw = (double)w * sc, dh = (double)h * sc;
    CGRect dst = CGRectMake((d->w - dw) / 2.0, (d->h - dh) / 2.0, dw, dh);

    CGContextDrawImage(d->ctx, dst, img);
    CGContextEndPage(d->ctx);
    CGImageRelease(img);
    return 0;
}

int pdf_add_text_page(PDFDoc *d, const char *utf8, double ptSize)
{
    if (!d || !utf8) return -1;
    if (ptSize <= 0.0) ptSize = 12.0;

    CFStringRef str = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
    if (!str) return -1;

    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), ptSize, NULL);
    CFStringRef keys[1] = { kCTFontAttributeName };
    CFTypeRef   vals[1] = { font };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)keys, (const void **)vals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CFRelease(attrs); CFRelease(font); CFRelease(str);
    if (!as) return -1;

    CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(as);
    CFRelease(as);
    if (!fs) return -1;

    const double margin = 54.0;
    CGRect textRect = CGRectMake(margin, margin, d->w - 2 * margin, d->h - 2 * margin);
    CGPathRef path = CGPathCreateWithRect(textRect, NULL);
    CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, NULL);

    CGRect media = CGRectMake(0, 0, d->w, d->h);
    CGContextBeginPage(d->ctx, &media);
    /* PDF context is y-up with origin bottom-left; CoreText fills the frame from the
     * top of textRect downward in that space — no flip needed. */
    CTFrameDraw(frame, d->ctx);
    CGContextEndPage(d->ctx);

    CFRelease(frame);
    CGPathRelease(path);
    CFRelease(fs);
    return 0;
}

int pdf_finish(PDFDoc *d)
{
    if (!d) return -1;
    CGPDFContextClose(d->ctx);
    CGContextRelease(d->ctx);
    free(d);
    return 0;
}

int pdf_raster_to_pdf(const char *pdfPath, const void *rgba, int w, int h)
{
    PDFDoc *d = pdf_begin(pdfPath);
    if (!d) return -1;
    int r = pdf_add_raster_page(d, rgba, w, h);
    int f = pdf_finish(d);
    return (r == 0 && f == 0) ? 0 : -1;
}
