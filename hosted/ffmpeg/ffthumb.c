/* ffthumb.c -- headless one-frame thumbnailer for AROS, powered by the native
 * ffmpeg port. Opens any supported image or video through the dos-backed custom
 * AVIOContext (aros_avio), decodes the first video frame (optionally skipping a
 * few frames past black intros), converts + downscales to a bounded RGB24
 * image, and writes it as a binary PPM (P6).
 *
 *   FFThumb <in> <out.ppm> [maxpx] [skipframes]
 *
 *     maxpx       longest-edge bound for the output (default 512, never
 *                 upscales)
 *     skipframes  decode this many extra frames first and thumbnail the last
 *                 one that decoded (default 0; callers pass ~8 for videos so
 *                 the poster is not a black fade-in frame)
 *
 * Exit 0 + "FFTHUMB: OK WxH" on stdout on success; nonzero + a one-line
 * reason on failure. Built for shelling out from a file manager's preview
 * worker (Feraille) exactly like macOS previews shell out to qlmanage: the
 * decoder runs in ITS OWN process, so codec crashes (h264/hevc are still WIP
 * on this target) never take the caller down.
 *
 * The YUV->RGB24 convert+downscale is the same hand-rolled fixed-point
 * nearest-neighbour path as ffview.c, for the same reason: libswscale's
 * yuv2rgb writer faults on this target, and every common video codec decodes
 * to planar YUV. sws is used only for non-YUV sources (RGB/paletted images).
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include "aros_avio.h"

#include <stdlib.h>
#include <string.h>

static AVFormatContext *g_fmt;
static AVCodecContext  *g_ctx;
static AVFrame  *g_frame;
static AVPacket *g_pkt;
static int g_vs;

static void msg(const char *s) { PutStr((CONST_STRPTR)s); }

static const char *udec(unsigned v, char *buf)
{
    char tmp[12]; int i = 0, j = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0'; return buf;
}

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

static unsigned char clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (unsigned char)v); }

/* Planar 8-bit YUV -> RGB24 with combined nearest-neighbour downscale
 * (16.16 fixed point), full/limited range BT.601. Same math as ffview.c.
 * Returns 1 if handled, 0 if the frame is not supported planar YUV. */
static int yuv_to_rgb24(unsigned char *dst, int dw, int dh)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(g_frame->format);
    const unsigned char *Y, *U, *V;
    int yls, uls, vls, cw, ch, full, dx, dy, xstep, ystep, syf;
    int w = g_frame->width, h = g_frame->height;

    if (!d || d->nb_components < 3 || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) ||
        (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) || d->comp[0].depth != 8)
        return 0;

    Y = g_frame->data[0]; U = g_frame->data[1]; V = g_frame->data[2];
    yls = g_frame->linesize[0]; uls = g_frame->linesize[1]; vls = g_frame->linesize[2];
    cw  = d->log2_chroma_w; ch = d->log2_chroma_h;
    full = (g_frame->color_range == AVCOL_RANGE_JPEG)
        || g_frame->format == AV_PIX_FMT_YUVJ420P || g_frame->format == AV_PIX_FMT_YUVJ422P
        || g_frame->format == AV_PIX_FMT_YUVJ444P || g_frame->format == AV_PIX_FMT_YUVJ440P;

    xstep = (w << 16) / dw;
    ystep = (h << 16) / dh;
    syf = 0;
    for (dy = 0; dy < dh; dy++) {
        int sy = syf >> 16; syf += ystep;
        const unsigned char *yr = Y + sy * yls;
        const unsigned char *ur = U + (sy >> ch) * uls;
        const unsigned char *vr = V + (sy >> ch) * vls;
        unsigned char *o = dst + (long)dy * dw * 3;
        int sxf = 0;
        for (dx = 0; dx < dw; dx++) {
            int sx = sxf >> 16; sxf += xstep;
            int yy = yr[sx], uu = ur[sx >> cw] - 128, vv = vr[sx >> cw] - 128, r, g, b;
            if (full) {
                r = yy + ((91881 * vv) >> 16);
                g = yy - ((22554 * uu + 46802 * vv) >> 16);
                b = yy + ((116130 * uu) >> 16);
            } else {
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

static void media_close(void)
{
    if (g_frame) av_frame_unref(g_frame);
    if (g_pkt)   av_packet_unref(g_pkt);
    if (g_ctx)   avcodec_free_context(&g_ctx);
    if (g_fmt)   { aros_avio_close(g_fmt); g_fmt = NULL; }
}

int main(int argc, char **argv)
{
    const char *in, *out;
    int maxpx = 512, skip = 0, i;
    int w, h, dw, dh, ok = 0;
    unsigned char *rgb = NULL;
    const AVCodec *dec = NULL;
    BPTR f;
    char hdr[48], num[16], *p;

    if (argc < 3) { msg("usage: FFThumb <in> <out.ppm> [maxpx] [skipframes]\n"); return RETURN_FAIL; }
    in = argv[1]; out = argv[2];
    /* Tolerate the unix-join artifact "dev:/path" (means parent-of-root to
     * DOS): collapse the slash right after the device colon in place. */
    {
        char *c = strchr((char *)in, ':');
        if (c && c[1] == '/') memmove(c + 1, c + 2, strlen(c + 2) + 1);
    }
    if (argc > 3) { maxpx = atoi(argv[3]); if (maxpx < 16) maxpx = 16; if (maxpx > 4096) maxpx = 4096; }
    if (argc > 4) { skip = atoi(argv[4]); if (skip < 0) skip = 0; if (skip > 300) skip = 300; }

    g_frame = av_frame_alloc();
    g_pkt   = av_packet_alloc();
    if (!g_frame || !g_pkt) { msg("FFThumb: alloc failed\n"); return RETURN_FAIL; }

    g_fmt = aros_avio_open(in);
    if (!g_fmt) { msg("FFThumb: cannot open input\n"); return RETURN_FAIL; }

    g_vs = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (g_vs < 0 || !dec) { msg("FFThumb: no video stream\n"); media_close(); return RETURN_FAIL; }
    g_ctx = avcodec_alloc_context3(dec);
    if (!g_ctx || avcodec_parameters_to_context(g_ctx, g_fmt->streams[g_vs]->codecpar) < 0
               || avcodec_open2(g_ctx, dec, NULL) < 0)
        { msg("FFThumb: codec open failed\n"); media_close(); return RETURN_FAIL; }

    if (!decode_next()) { msg("FFThumb: no frame decoded\n"); media_close(); return RETURN_FAIL; }
    /* Skip past intro frames; keep the last one that decoded. */
    for (i = 0; i < skip; i++) {
        av_frame_unref(g_frame);
        if (!decode_next()) break;
    }
    if (!g_frame->data[0]) { msg("FFThumb: no frame data\n"); media_close(); return RETURN_FAIL; }

    w = g_frame->width; h = g_frame->height;
    if (w < 1 || h < 1) { msg("FFThumb: bad frame size\n"); media_close(); return RETURN_FAIL; }
    dw = w; dh = h;                                /* bound longest edge, never upscale */
    if (dw > maxpx) { dh = (int)((long)dh * maxpx / dw); dw = maxpx; }
    if (dh > maxpx) { dw = (int)((long)dw * maxpx / dh); dh = maxpx; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    rgb = malloc((long)dw * dh * 3);
    if (!rgb) { msg("FFThumb: out of memory\n"); media_close(); return RETURN_FAIL; }

    if (yuv_to_rgb24(rgb, dw, dh)) {
        ok = 1;
    } else {
        /* Non-YUV source (RGB/paletted image): sws is safe here — the
         * target-specific fault is only in its yuv2rgb writer. */
        struct SwsContext *sws = sws_getContext(w, h, g_frame->format, dw, dh,
                                                AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                                NULL, NULL, NULL);
        if (sws) {
            unsigned char *dst[4] = { rgb, NULL, NULL, NULL };
            int dls[4] = { dw * 3, 0, 0, 0 };
            if (sws_scale(sws, (const unsigned char * const *)g_frame->data,
                          g_frame->linesize, 0, h, dst, dls) == dh)
                ok = 1;
            sws_freeContext(sws);
        }
    }
    if (!ok) { msg("FFThumb: convert failed\n"); free(rgb); media_close(); return RETURN_FAIL; }

    /* P6 header + pixels. DOS Write in one row-tight buffer (no padding). */
    p = hdr;
    *p++ = 'P'; *p++ = '6'; *p++ = '\n';
    { const char *q = udec((unsigned)dw, num); while (*q) *p++ = *q++; }
    *p++ = ' ';
    { const char *q = udec((unsigned)dh, num); while (*q) *p++ = *q++; }
    *p++ = '\n'; *p++ = '2'; *p++ = '5'; *p++ = '5'; *p++ = '\n';

    f = Open((CONST_STRPTR)out, MODE_NEWFILE);
    if (!f) { msg("FFThumb: cannot open output\n"); free(rgb); media_close(); return RETURN_FAIL; }
    if (Write(f, hdr, p - hdr) != p - hdr
        || Write(f, rgb, (long)dw * dh * 3) != (long)dw * dh * 3) {
        Close(f); free(rgb); media_close();
        msg("FFThumb: write failed\n");
        return RETURN_FAIL;
    }
    Close(f);
    free(rgb);
    media_close();

    msg("FFTHUMB: OK ");
    msg(udec((unsigned)dw, num)); msg("x");
    msg(udec((unsigned)dh, num)); msg("\n");
    return RETURN_OK;
}
