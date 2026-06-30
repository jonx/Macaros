/* ffview.c -- a simple image / video viewer for AROS, powered by ffmpeg built
 * natively for AROS. Opens any supported image or video (mp4/avi/mkv/mov/gif/...)
 * through the dos-backed custom AVIOContext (aros_avio), decodes with libavcodec,
 * scales to RGB24 with libswscale, and shows it in an intuition window. Video
 * plays back paced by timer.device with basic controls.
 *
 *   FFView <file>
 *
 * Controls (window must be active):
 *   SPACE   play / pause          R   restart from the first frame
 *   Q / ESC quit                  close gadget quits
 * The title bar shows PLAYING / PAUSED and the frame count.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include "aros_avio.h"

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

/* Convert g_frame to RGB24 and blit into the window's inner area. */
static void blit(void)
{
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

int main(int argc, char **argv)
{
    const AVCodec *dec = NULL;
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    BOOL have_timer = FALSE, timer_pending = FALSE;
    BOOL playing = TRUE, quit = FALSE, is_image = FALSE;
    ULONG winsig, tsig, frame_usec = 66666;   /* ~15fps fallback */
    int rc = 20;

    if (argc < 2) { msg("usage: FFView <file>\n"); return 5; }
    if (argc > 2 && argv[2] && argv[2][0]) g_trace = Open((CONST_STRPTR)argv[2], MODE_NEWFILE);
    trace("start");

    g_fmt = aros_avio_open(argv[1]);
    if (!g_fmt) { msg("FFView: cannot open "); msg(argv[1]); msg("\n"); return 20; }

    g_vs = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (g_vs < 0 || !dec) { msg("FFView: no video stream\n"); goto cleanup; }
    g_ctx = avcodec_alloc_context3(dec);
    if (!g_ctx || avcodec_parameters_to_context(g_ctx, g_fmt->streams[g_vs]->codecpar) < 0
               || avcodec_open2(g_ctx, dec, NULL) < 0) { msg("FFView: codec open failed\n"); goto cleanup; }

    /* frame pace from the stream's average rate */
    {
        AVRational fr = g_fmt->streams[g_vs]->avg_frame_rate;
        if (fr.num > 0 && fr.den > 0)
            frame_usec = (ULONG)((1000000.0 * fr.den) / fr.num);
    }

    g_pkt = av_packet_alloc();
    g_frame = av_frame_alloc();
    if (!g_pkt || !g_frame) { msg("FFView: alloc failed\n"); goto cleanup; }

    trace("codec-open");
    if (!decode_next()) { msg("FFView: no frame decoded\n"); goto cleanup; }
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

    g_win = OpenWindowTags(NULL,
        WA_Title,       (IPTR)"ffview",
        WA_InnerWidth,  (IPTR)g_dw,
        WA_InnerHeight, (IPTR)g_dh,
        WA_Left,        (IPTR)4, WA_Top, (IPTR)14,
        WA_Flags,       (IPTR)(WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE),
        WA_IDCMP,       (IPTR)(IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY),
        TAG_DONE);
    if (!g_win) { msg("FFView: OpenWindow failed\n"); goto cleanup; }
    trace("window-open");
    blit();                                   /* show the first frame */
    trace("blit-first");

    /* timer.device for frame pacing */
    tport = CreateMsgPort();
    if (tport) {
        treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
        if (treq && OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                               (struct IORequest *)treq, 0) == 0)
            have_timer = TRUE;
    }

    winsig = 1UL << g_win->UserPort->mp_SigBit;
    tsig   = have_timer ? (1UL << tport->mp_SigBit) : 0;

    set_title(playing, FALSE);
    trace(have_timer ? "timer-ok loop" : "no-timer loop");
    rc = 0;

    /* kick the first inter-frame delay */
    if (have_timer && playing) {
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs  = frame_usec / 1000000;
        treq->tr_time.tv_micro = frame_usec % 1000000;
        SendIO((struct IORequest *)treq);
        timer_pending = TRUE;
    }

    while (!quit) {
        ULONG sigs = Wait(winsig | tsig | SIGBREAKF_CTRL_C);
        struct IntuiMessage *im;

        if (sigs & SIGBREAKF_CTRL_C) quit = TRUE;

        while ((im = (struct IntuiMessage *)GetMsg(g_win->UserPort))) {
            ULONG cls = im->Class; UWORD code = im->Code;
            ReplyMsg((struct Message *)im);
            if (cls == IDCMP_CLOSEWINDOW) quit = TRUE;
            else if (cls == IDCMP_VANILLAKEY) {
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
                } else if (code == 'q' || code == 'Q' || code == 0x1B) {
                    quit = TRUE;
                }
            }
        }

        if ((sigs & tsig) && timer_pending && CheckIO((struct IORequest *)treq)) {
            WaitIO((struct IORequest *)treq);
            timer_pending = FALSE;
            if (playing && !is_image) {
                if (!decode_next()) {                 /* hit EOF */
                    if (g_count <= 1) { is_image = TRUE; set_title(FALSE, TRUE); }
                    else { seek_start(); if (decode_next()) blit(); }
                } else {
                    blit();
                }
                if ((g_count % 12) == 0) trace("tick");
                if (!is_image) {
                    treq->tr_node.io_Command = TR_ADDREQUEST;
                    treq->tr_time.tv_secs  = frame_usec / 1000000;
                    treq->tr_time.tv_micro = frame_usec % 1000000;
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
    if (g_win) CloseWindow(g_win);
    if (g_rgbframe) av_frame_free(&g_rgbframe);
    if (g_sws) sws_freeContext(g_sws);
    if (g_frame) av_frame_free(&g_frame);
    if (g_pkt) av_packet_free(&g_pkt);
    if (g_ctx) avcodec_free_context(&g_ctx);
    if (g_fmt) aros_avio_close(g_fmt);
    return rc;
}
