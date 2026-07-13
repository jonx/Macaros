/* ffview.c -- a simple image / video viewer for AROS, powered by ffmpeg built
 * natively for AROS. Opens any supported image or video (mp4/avi/mkv/mov/gif/...)
 * through the dos-backed custom AVIOContext (aros_avio), decodes with libavcodec,
 * scales to RGB24 with libswscale, and shows it in an intuition window. Video
 * plays back paced by timer.device with basic controls.
 *
 *   FFView [<file>]
 *
 * The window is a Workbench AppWindow: drop a file icon from Wanderer on it to
 * open that file in the viewer (with no argument it opens empty, waiting for a
 * drop).
 *
 * Controls (window must be active):
 *   SPACE   play / pause          R   restart from the first frame
 *   I       toggle a stats overlay (frame, size, codec, time) burned into the
 *           image, so it shows in a screen recording
 *   Q / ESC quit                  close gadget quits
 * The title bar shows PLAYING / PAUSED and the frame count.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/wb.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <workbench/startup.h>       /* struct WBArg (the drop payload) */
#include <workbench/workbench.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include "aros_avio.h"

/* gpufx.library (docs/features/gpufx): GPU YUV420->RGBA + scale, sharing the
   display's Metal device. Opened lazily; when present and the frame is planar
   4:2:0, the per-frame colour convert (and any downscale-to-fit) run on the GPU
   -- 5-7x the scalar path -- with the library's own CPU fallback underneath, so
   there is no behaviour change when it is absent. */
#include <libraries/gpufx.h>
#include <proto/gpufx.h>
struct Library *GfxFxBase;

/* workbench.library, for the AppWindow (drag-and-drop) registration. Opened
   explicitly; when absent the viewer simply is not a drop target. */
struct Library *WorkbenchBase;

/* ---- decode/display state ---- */
static AVFormatContext *g_fmt;
static AVCodecContext  *g_ctx;
static AVFrame  *g_frame;
static AVPacket *g_pkt;
static struct SwsContext *g_sws;
static AVFrame *g_rgbframe;       /* RGB24 dst, av_frame_get_buffer-padded for sws */
static int g_vs;                 /* video stream index */
static int g_w, g_h;             /* decoded frame size */
static int g_dw, g_dh;           /* on-screen size (downscaled to fit the screen) */
static struct Window *g_win;
static long g_count;             /* frames shown so far */
static BOOL g_showstats;         /* 'i' toggles a stats line burned into the frame */
static const char *g_decname = "?";
static int g_fps;                /* nominal stream fps (for the time readout) */
static ULONG g_frame_usec = 66666;  /* inter-frame delay; ~15fps fallback */

static void msg(const char *s) { PutStr((CONST_STRPTR)s); }

/* optional progress trace to argv[2] (debug aid; Flush so it survives a hang) */
static BPTR g_trace = 0;
static void trace(const char *s) { if (g_trace) { FPuts(g_trace, (CONST_STRPTR)s); FPutC(g_trace, '\n'); Flush(g_trace); } }

/* Pull the next decodable video frame into g_frame. 1 = got one, 0 = EOF. */
static int decode_next(void)
{
    for (;;) {
        int r = av_read_frame(g_fmt, g_pkt);
        if (r < 0) {
            avcodec_send_packet(g_ctx, NULL);              /* drain at EOF */
            if (avcodec_receive_frame(g_ctx, g_frame) == 0) return 1;
            return 0;
        }
        if (g_pkt->stream_index == g_vs &&
            avcodec_send_packet(g_ctx, g_pkt) == 0) {
            int rr = avcodec_receive_frame(g_ctx, g_frame);
            if (rr == 0) { av_packet_unref(g_pkt); return 1; }
        }
        av_packet_unref(g_pkt);
    }
}

static void seek_start(void)
{
    av_seek_frame(g_fmt, g_vs, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(g_ctx);
}

static unsigned char clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v); }

/* Hand-written planar-YUV -> RGB24 for any 8-bit planar YUV (4:2:0, 4:2:2, 4:4:4
   ...), using the chroma subsampling from the format descriptor. libswscale's
   yuv2rgb24 writer faults on this target (a bug in the cross-built C output
   path), and every common video codec decodes to a planar YUV, so we convert
   directly. JPEG/mjpeg is full-range; others limited-range BT.601. Returns 1 if
   it handled the frame, 0 if the format is not supported planar YUV. */
static int yuv_to_rgb24(void)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(g_frame->format);
    const unsigned char *Y, *U, *V;
    int yls, uls, vls, ds, cw, ch, full, dx, dy, xstep, ystep, syf;
    unsigned char *o0;

    if (!d || d->nb_components < 3 || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) ||
        (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) || d->comp[0].depth != 8)
        return 0;

    Y = g_frame->data[0]; U = g_frame->data[1]; V = g_frame->data[2];
    yls = g_frame->linesize[0]; uls = g_frame->linesize[1]; vls = g_frame->linesize[2];
    ds  = g_rgbframe->linesize[0]; o0 = g_rgbframe->data[0];
    cw  = d->log2_chroma_w; ch = d->log2_chroma_h;
    full = (g_frame->color_range == AVCOL_RANGE_JPEG)
        || g_frame->format == AV_PIX_FMT_YUVJ420P || g_frame->format == AV_PIX_FMT_YUVJ422P
        || g_frame->format == AV_PIX_FMT_YUVJ444P || g_frame->format == AV_PIX_FMT_YUVJ440P;

    /* 16.16 fixed-point source step per dest pixel: == 1.0 when not scaling,
       > 1.0 when downscaling to fit the screen. Nearest-neighbour. */
    xstep = (g_w << 16) / g_dw;
    ystep = (g_h << 16) / g_dh;
    syf = 0;
    for (dy = 0; dy < g_dh; dy++) {
        int sy = syf >> 16; syf += ystep;
        const unsigned char *yr = Y + sy * yls;
        const unsigned char *ur = U + (sy >> ch) * uls;
        const unsigned char *vr = V + (sy >> ch) * vls;
        unsigned char *o = o0 + dy * ds;
        int sxf = 0;
        for (dx = 0; dx < g_dw; dx++) {
            int sx = sxf >> 16; sxf += xstep;
            int yy = yr[sx], uu = ur[sx >> cw] - 128, vv = vr[sx >> cw] - 128, r, g, b;
            if (full) {                                  /* full-range (JPEG) BT.601 */
                r = yy + ((91881 * vv) >> 16);
                g = yy - ((22554 * uu + 46802 * vv) >> 16);
                b = yy + ((116130 * uu) >> 16);
            } else {                                     /* limited-range BT.601 */
                int c = 76284 * (yy - 16);
                r = (c + 104595 * vv) >> 16;
                g = (c - 25624 * uu - 53281 * vv) >> 16;
                b = (c + 132251 * uu) >> 16;
            }
            o[0] = clamp8(r); o[1] = clamp8(g); o[2] = clamp8(b);
            o += 3;
        }
    }
    return 1;
}

static const char *udec(unsigned v, char *buf)
{
    char tmp[12]; int i = 0, j = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0'; return buf;
}
static char *apps(char *p, const char *q) { while (*q) *p++ = *q++; return p; }

/* Burn a one-line stats overlay into the just-blitted frame: graphics.library
   Text on the window rastport, so it is part of the on-screen image and shows up
   in a screen recording. Toggled with 'i'. */
static void draw_stats(void)
{
    struct RastPort *rp = g_win->RPort;
    char s[160], num[16], *p = s;
    int secs = g_fps > 0 ? (int)(g_count / g_fps) : 0;

    p = apps(p, "frame "); p = apps(p, udec((unsigned)g_count, num));
    p = apps(p, "  ");     p = apps(p, udec(g_w, num)); *p++ = 'x'; p = apps(p, udec(g_h, num));
    p = apps(p, "  ");     p = apps(p, g_decname);
    p = apps(p, "  ");     p = apps(p, udec((unsigned)secs, num)); *p++ = 's';
    if (g_fps > 0) { p = apps(p, " @"); p = apps(p, udec((unsigned)g_fps, num)); p = apps(p, "fps"); }
    *p = '\0';

    SetDrMd(rp, JAM2);
    SetBPen(rp, 1);                 /* background (dark UI pen) */
    SetAPen(rp, 2);                 /* text (light UI pen) */
    Move(rp, g_win->BorderLeft + 4, g_win->BorderTop + 9);
    Text(rp, (CONST_STRPTR)s, (LONG)(p - s));
}

/* GPU path (gpufx.library): planar 4:2:0 -> RGBA on the GPU, plus a GPU
   bilinear downscale when the video does not fit the window, then blit RGBA.
   Returns 1 if it handled the frame, 0 to fall back to the scalar/sws path.
   Buffers are grown as needed and freed in cleanup(). */
static unsigned char *g_gpu_conv;   /* source-size RGBA (convert dst)      */
static unsigned char *g_gpu_disp;   /* display-size RGBA (scale dst)       */
static int g_gpu_conv_cap, g_gpu_disp_cap;

static int gpufx_blit(void)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(g_frame->format);
    int need, full, scaled;
    unsigned char *rgba; int rgba_stride;
    struct GfxFxYuvReq yr;

    if (!GfxFxBase || !d)
        return 0;
    /* Exactly planar 8-bit 4:2:0 (what cm_gpu_convert_yuv420 assumes). */
    if (!(d->flags & AV_PIX_FMT_FLAG_PLANAR) || d->nb_components < 3
        || d->comp[0].depth != 8 || d->log2_chroma_w != 1 || d->log2_chroma_h != 1
        || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)))
        return 0;

    full = (g_frame->color_range == AVCOL_RANGE_JPEG)
        || g_frame->format == AV_PIX_FMT_YUVJ420P;

    /* convert at source size into g_gpu_conv */
    need = g_w * g_h * 4;
    if (g_gpu_conv_cap < need) {
        FreeVec(g_gpu_conv);
        g_gpu_conv = AllocVec(need, MEMF_ANY);
        g_gpu_conv_cap = g_gpu_conv ? need : 0;
    }
    if (!g_gpu_conv) return 0;

    yr.y = g_frame->data[0]; yr.u = g_frame->data[1]; yr.v = g_frame->data[2];
    yr.rgba = g_gpu_conv;
    yr.yStride = g_frame->linesize[0]; yr.uStride = g_frame->linesize[1];
    yr.vStride = g_frame->linesize[2];
    yr.w = g_w; yr.h = g_h; yr.dstStride = g_w * 4; yr.fullRange = full;
    if (GfxFx_ConvertYUV420(&yr) != 0)
        return 0;

    scaled = (g_dw != g_w || g_dh != g_h);
    if (scaled) {
        struct GfxFxScaleReq sc;
        need = g_dw * g_dh * 4;
        if (g_gpu_disp_cap < need) {
            FreeVec(g_gpu_disp);
            g_gpu_disp = AllocVec(need, MEMF_ANY);
            g_gpu_disp_cap = g_gpu_disp ? need : 0;
        }
        if (!g_gpu_disp) return 0;
        sc.src = g_gpu_conv; sc.dst = g_gpu_disp;
        sc.srcStride = g_w * 4; sc.sw = g_w; sc.sh = g_h;
        sc.dstStride = g_dw * 4; sc.dw = g_dw; sc.dh = g_dh; sc.filter = 1;
        if (GfxFx_Scale(&sc) != 0)
            return 0;
        rgba = g_gpu_disp; rgba_stride = g_dw * 4;
    } else {
        rgba = g_gpu_conv; rgba_stride = g_w * 4;
    }

    WritePixelArray(rgba, 0, 0, rgba_stride, g_win->RPort,
                    g_win->BorderLeft, g_win->BorderTop, g_dw, g_dh, RECTFMT_RGBA);
    g_count++;
    if (g_showstats) draw_stats();
    return 1;
}

/* Convert g_frame to RGB24 and blit into the window's inner area. */
static void blit(void)
{
    if (gpufx_blit())        /* GPU fast path (planar 4:2:0); else scalar/sws */
        return;

    if (!g_rgbframe) {
        g_rgbframe = av_frame_alloc();
        if (!g_rgbframe) return;
        g_rgbframe->format = AV_PIX_FMT_RGB24;
        g_rgbframe->width  = g_dw;                     /* on-screen (possibly downscaled) size */
        g_rgbframe->height = g_dh + 16;                /* slack for any sws-path overrun */
        if (av_frame_get_buffer(g_rgbframe, 64) < 0) { av_frame_free(&g_rgbframe); return; }
        g_rgbframe->height = g_dh;
    }

    if (!yuv_to_rgb24()) {                             /* non-YUV (e.g. RGB/paletted image) */
        if (!g_sws)
            g_sws = sws_getContext(g_w, g_h, g_frame->format, g_dw, g_dh, AV_PIX_FMT_RGB24,
                                   SWS_BILINEAR, NULL, NULL, NULL);
        if (g_sws)
            sws_scale(g_sws, (const unsigned char * const *)g_frame->data, g_frame->linesize,
                      0, g_h, g_rgbframe->data, g_rgbframe->linesize);
    }
    WritePixelArray(g_rgbframe->data[0], 0, 0, g_rgbframe->linesize[0], g_win->RPort,
                    g_win->BorderLeft, g_win->BorderTop, g_dw, g_dh, RECTFMT_RGB);
    g_count++;
    if (g_showstats) draw_stats();
}

static void set_title(BOOL playing, BOOL image)
{
    static char t[96];
    char *p = t;
    const char *s = image ? "ffview - image" : (playing ? "ffview - PLAYING" : "ffview - PAUSED");
    while (*s) *p++ = *s++;
    *p = '\0';
    SetWindowTitles(g_win, (CONST_STRPTR)t, (CONST_STRPTR)~0UL);
}

/* ---- media open/close (extracted so a drag-and-drop can swap files) ---- */

/* Everything that belongs to the open file. g_pkt/g_frame live for the whole
   program (they are per-call scratch for libavcodec). */
static void media_close(void)
{
    if (g_frame) av_frame_unref(g_frame);
    if (g_pkt)   av_packet_unref(g_pkt);
    if (g_rgbframe) av_frame_free(&g_rgbframe);
    if (g_sws)   { sws_freeContext(g_sws); g_sws = NULL; }
    if (g_ctx)   avcodec_free_context(&g_ctx);
    if (g_fmt)   { aros_avio_close(g_fmt); g_fmt = NULL; }
    g_count = 0;
    g_decname = "?";
}

/* Open <path>, pick the video stream, decode the first frame and compute the
   on-screen size. On failure everything is closed again and FALSE returned. */
static BOOL media_open(const char *path)
{
    const AVCodec *dec = NULL;

    g_fmt = aros_avio_open(path);
    if (!g_fmt) { msg("FFView: cannot open "); msg(path); msg("\n"); return FALSE; }

    g_vs = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (g_vs < 0 || !dec) { msg("FFView: no video stream\n"); media_close(); return FALSE; }
    g_ctx = avcodec_alloc_context3(dec);
    if (!g_ctx || avcodec_parameters_to_context(g_ctx, g_fmt->streams[g_vs]->codecpar) < 0
               || avcodec_open2(g_ctx, dec, NULL) < 0)
        { msg("FFView: codec open failed\n"); media_close(); return FALSE; }
    g_decname = (dec && dec->name) ? dec->name : "?";

    /* frame pace from the stream's average rate */
    g_frame_usec = 66666;                     /* ~15fps fallback */
    {
        AVRational fr = g_fmt->streams[g_vs]->avg_frame_rate;
        if (fr.num > 0 && fr.den > 0)
            g_frame_usec = (ULONG)((1000000.0 * fr.den) / fr.num);
    }
    g_fps = g_frame_usec ? (int)(1000000 / g_frame_usec) : 0;

    trace("codec-open");
    if (!decode_next()) { msg("FFView: no frame decoded\n"); media_close(); return FALSE; }
    g_w = g_frame->width; g_h = g_frame->height;
    /* Downscale (preserving aspect, never up) to fit the 800x600 screen minus
       title/borders + WA_Left/Top. The blit converter samples at this size. */
    {
        const int mw = 740, mh = 536;
        g_dw = g_w; g_dh = g_h;
        if (g_dw > mw) { g_dh = (int)((long)g_dh * mw / g_dw); g_dw = mw; }
        if (g_dh > mh) { g_dw = (int)((long)g_dw * mh / g_dh); g_dh = mh; }
        if (g_dw < 1) g_dw = 1;
        if (g_dh < 1) g_dh = 1;
    }
    trace("decoded-first");
    return TRUE;
}

/* ---- window open/close (reopened when a drop changes the frame size) ---- */

static struct MsgPort *g_awport;        /* AppWindow drop messages */
static struct AppWindow *g_appwin;
static int g_winx = 4, g_winy = 14;     /* kept across reopens */

#define EMPTY_W 300
#define EMPTY_H 60

static void window_close(void)
{
    if (g_appwin) { RemoveAppWindow(g_appwin); g_appwin = NULL; }
    if (g_awport) {                     /* reply anything still queued */
        struct Message *m;
        while ((m = GetMsg(g_awport))) ReplyMsg(m);
    }
    if (g_win) {
        g_winx = g_win->LeftEdge; g_winy = g_win->TopEdge;
        CloseWindow(g_win);
        g_win = NULL;
    }
}

/* Open the viewer window: at the media size, or the small "drop a file here"
   hint when no media is open. Registers it as an AppWindow when possible. */
static BOOL window_open(void)
{
    int iw = g_fmt ? g_dw : EMPTY_W;
    int ih = g_fmt ? g_dh : EMPTY_H;

    g_win = OpenWindowTags(NULL,
        WA_Title,       (IPTR)"ffview",
        WA_InnerWidth,  (IPTR)iw,
        WA_InnerHeight, (IPTR)ih,
        WA_Left,        (IPTR)g_winx, WA_Top, (IPTR)g_winy,
        WA_Flags,       (IPTR)(WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE),
        WA_IDCMP,       (IPTR)(IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY),
        TAG_DONE);
    if (!g_win) { msg("FFView: OpenWindow failed\n"); return FALSE; }

    if (WorkbenchBase && g_awport)
        g_appwin = AddAppWindow(0, 0, g_win, g_awport, NULL);

    if (!g_fmt) {                       /* empty state: draw the hint */
        struct RastPort *rp = g_win->RPort;
        static const char hint[] = "drop an image or video here";
        SetDrMd(rp, JAM1);
        SetAPen(rp, 1);
        Move(rp, g_win->BorderLeft + 16, g_win->BorderTop + (EMPTY_H / 2) + 4);
        Text(rp, (CONST_STRPTR)hint, sizeof(hint) - 1);
        SetWindowTitles(g_win, (CONST_STRPTR)"ffview - drop a file",
                        (CONST_STRPTR)~0UL);
    }
    trace("window-open");
    return TRUE;
}

/* A file icon was dropped on the window: switch the viewer to it. The window
   is reopened because the frame size (and so the inner window size) changes. */
static BOOL drop_switch(const char *path, BOOL *playing, BOOL *is_image)
{
    trace("drop");
    window_close();
    media_close();

    *playing = FALSE; *is_image = TRUE;       /* nothing to pace on failure */
    if (!media_open(path)) {
        if (!window_open()) return FALSE;     /* empty window + hint again */
        SetWindowTitles(g_win, (CONST_STRPTR)"ffview - cannot open that file",
                        (CONST_STRPTR)~0UL);
        return TRUE;
    }
    if (!window_open()) return FALSE;
    *playing = TRUE; *is_image = FALSE;
    blit();                                   /* show the first frame */
    set_title(TRUE, FALSE);
    return TRUE;
}

int main(int argc, char **argv)
{
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL have_timer = FALSE, timer_pending = FALSE;
    BOOL playing = TRUE, quit = FALSE, is_image = FALSE;
    ULONG winsig, awsig, tsig;
    int rc = 20;

    if (argc > 2 && argv[2] && argv[2][0]) g_trace = Open((CONST_STRPTR)argv[2], MODE_NEWFILE);
    trace("start");

    /* Optional GPU colour-convert/scale. Absent => the scalar path runs
       unchanged, so this never gates playback. */
    GfxFxBase = OpenLibrary((CONST_STRPTR)"gpufx.library", 0);
    if (GfxFxBase && GfxFx_Available())
        trace("gpufx: GPU path available");

    /* Drag-and-drop support (optional: no Workbench, no drop target) */
    WorkbenchBase = OpenLibrary((CONST_STRPTR)"workbench.library", 0);
    if (WorkbenchBase) g_awport = CreateMsgPort();

    g_pkt = av_packet_alloc();
    g_frame = av_frame_alloc();
    if (!g_pkt || !g_frame) { msg("FFView: alloc failed\n"); goto cleanup; }

    if (argc >= 2 && argv[1] && argv[1][0]) {
        if (!media_open(argv[1])) goto cleanup;
    } else if (!g_awport) {
        /* no file and no Workbench to drop one from: keep the old behavior */
        msg("usage: FFView <file>\n");
        rc = 5;
        goto cleanup;
    }

    if (!window_open()) goto cleanup;
    if (g_fmt) {
        blit();                               /* show the first frame */
        trace("blit-first");
    }

    /* timer.device for frame pacing */
    tport = CreateMsgPort();
    if (tport) {
        treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
        if (treq && OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                               (struct IORequest *)treq, 0) == 0)
            have_timer = TRUE;
    }

    winsig = 1UL << g_win->UserPort->mp_SigBit;
    awsig  = g_awport ? (1UL << g_awport->mp_SigBit) : 0;
    tsig   = have_timer ? (1UL << tport->mp_SigBit) : 0;

    if (g_fmt) set_title(playing, FALSE);
    trace(have_timer ? "timer-ok loop" : "no-timer loop");
    rc = 0;

    /* kick the first inter-frame delay */
    if (g_fmt && have_timer && playing) {
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs  = g_frame_usec / 1000000;
        treq->tr_time.tv_micro = g_frame_usec % 1000000;
        SendIO((struct IORequest *)treq);
        timer_pending = TRUE;
    }

    while (!quit) {
        ULONG sigs = Wait(winsig | awsig | tsig | SIGBREAKF_CTRL_C);
        struct IntuiMessage *im;
        struct AppMessage *am;

        if (sigs & SIGBREAKF_CTRL_C) quit = TRUE;

        while ((im = (struct IntuiMessage *)GetMsg(g_win->UserPort))) {
            ULONG cls = im->Class; UWORD code = im->Code;
            ReplyMsg((struct Message *)im);
            if (cls == IDCMP_CLOSEWINDOW) quit = TRUE;
            else if (cls == IDCMP_VANILLAKEY && g_fmt) {
                if (code == ' ' && !is_image) {
                    playing = !playing; set_title(playing, FALSE);
                    if (playing && have_timer && !timer_pending) {
                        treq->tr_node.io_Command = TR_ADDREQUEST;
                        treq->tr_time.tv_secs = 0; treq->tr_time.tv_micro = 1;
                        SendIO((struct IORequest *)treq); timer_pending = TRUE;
                    }
                } else if (code == 'r' || code == 'R') {
                    seek_start();
                    if (decode_next()) blit();
                } else if (code == 'i' || code == 'I') {
                    g_showstats = !g_showstats;
                    blit();                       /* re-render current frame with/without the overlay */
                } else if (code == 'q' || code == 'Q' || code == 0x1B) {
                    quit = TRUE;
                }
            }
            else if (cls == IDCMP_VANILLAKEY &&
                     (code == 'q' || code == 'Q' || code == 0x1B)) {
                quit = TRUE;                      /* empty window: only quit works */
            }
        }

        /* a Wanderer icon was dropped on the window */
        if (g_awport) {
            char droppath[512]; BOOL dropped = FALSE;
            while ((am = (struct AppMessage *)GetMsg(g_awport))) {
                if (!dropped && am->am_NumArgs >= 1 && am->am_ArgList &&
                    am->am_ArgList[0].wa_Name && am->am_ArgList[0].wa_Name[0]) {
                    droppath[0] = '\0';
                    if (NameFromLock(am->am_ArgList[0].wa_Lock,
                                     (STRPTR)droppath, sizeof(droppath)) &&
                        AddPart((STRPTR)droppath,
                                (CONST_STRPTR)am->am_ArgList[0].wa_Name,
                                sizeof(droppath)))
                        dropped = TRUE;
                }
                ReplyMsg((struct Message *)am);
            }
            if (dropped && !quit) {
                if (!drop_switch(droppath, &playing, &is_image)) { quit = TRUE; rc = 20; }
                else {
                    winsig = 1UL << g_win->UserPort->mp_SigBit;
                    if (g_fmt && playing && have_timer && !timer_pending) {
                        treq->tr_node.io_Command = TR_ADDREQUEST;
                        treq->tr_time.tv_secs = 0; treq->tr_time.tv_micro = 1;
                        SendIO((struct IORequest *)treq); timer_pending = TRUE;
                    }
                }
            }
        }

        if ((sigs & tsig) && timer_pending && CheckIO((struct IORequest *)treq)) {
            WaitIO((struct IORequest *)treq);
            timer_pending = FALSE;
            if (g_fmt && playing && !is_image) {
                if (!decode_next()) {                 /* hit EOF */
                    if (g_count <= 1) { is_image = TRUE; set_title(FALSE, TRUE); }
                    else { seek_start(); if (decode_next()) blit(); }
                } else {
                    blit();
                }
                if ((g_count % 12) == 0) trace("tick");
                if (!is_image) {
                    treq->tr_node.io_Command = TR_ADDREQUEST;
                    treq->tr_time.tv_secs  = g_frame_usec / 1000000;
                    treq->tr_time.tv_micro = g_frame_usec % 1000000;
                    SendIO((struct IORequest *)treq); timer_pending = TRUE;
                }
            }
        }
    }

    trace("quit");
    if (have_timer && timer_pending) { AbortIO((struct IORequest *)treq); WaitIO((struct IORequest *)treq); }

cleanup:
    trace("cleanup");
    if (have_timer) CloseDevice((struct IORequest *)treq);
    if (treq)  DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    window_close();
    if (g_awport) DeleteMsgPort(g_awport);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    FreeVec(g_gpu_conv); FreeVec(g_gpu_disp);
    if (GfxFxBase) CloseLibrary(GfxFxBase);
    media_close();
    if (g_frame) av_frame_free(&g_frame);
    if (g_pkt) av_packet_free(&g_pkt);
    return rc;
}
