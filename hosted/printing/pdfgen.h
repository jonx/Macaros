/* pdfgen.h — flat C ABI for the host PDF-generation engine (print-to-PDF, the
 * PDF-direct path of the printing bridge).
 *
 * Implemented from docs/features/printing/spec.md ("The C ABI (cups_shim.h)" —
 * cups_raster_to_pdf, and the design's PDF-direct decision). Independent work: no
 * third-party implementation source — emulator, agent, driver, or otherwise — was
 * read, searched, or consulted in producing it, and any resemblance to existing
 * implementations is coincidental. Sources: Apple CoreGraphics CGPDFContext /
 * CoreText docs [PUB]; this project's flat-C host-shim shape
 * (hosted/coreaudio/coreaudio_shim.h) [OURS]. No AROS headers are pulled here.
 *
 * Why this exists: macOS 14 removed PostScript→PDF conversion, so the printing
 * bridge generates PDF on OUR side via CGPDFContext (the supported, non-deprecated
 * Quartz path) rather than asking the host to convert PostScript. This module is
 * that engine: it turns what printer.device produces — a rasterized page (the
 * graphics-dump / gfx engine) or a text stream — into a PDF on disk, headlessly,
 * with no print dialog and no TCC prompt.
 *
 * This is also the module the Macaros app can link/load directly: it is a single
 * .c + .h over CoreGraphics/CoreText with no AROS dependency, and it builds into
 * libpdfgen.dylib (dlopen'd by the AROS side via hostlib.resource).
 */
#ifndef PDFGEN_H
#define PDFGEN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PDFDoc PDFDoc;

/* Begin a multi-page PDF at `path` (US Letter, 612x792 pt). Returns NULL on
   failure. Pages are added in order; call pdf_finish() to flush + close. */
PDFDoc *pdf_begin(const char *path);

/* Add one page that draws an RGBA8888 raster (the printer.device graphics-dump
   path). `rgba` is w*h*4 bytes, premultiplied-alpha last; it must stay valid for
   the duration of this call. The image is fit to the page (aspect-preserved,
   centred, margin). Returns 0 on success. */
int pdf_add_raster_page(PDFDoc *, const void *rgba, int w, int h);

/* Add one page of real, selectable text laid out with CoreText (the printer.device
   text-stream path). `utf8` is the page text (newlines honoured); ptSize <= 0 uses
   a default. Returns 0 on success. */
int pdf_add_text_page(PDFDoc *, const char *utf8, double ptSize);

/* Flush, close, and free the document. Returns 0 on success. */
int pdf_finish(PDFDoc *);

/* One-shot convenience == the spec's cups_raster_to_pdf: a single-page PDF from one
   RGBA raster. Returns 0 on success. */
int pdf_raster_to_pdf(const char *pdfPath, const void *rgba, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* PDFGEN_H */
