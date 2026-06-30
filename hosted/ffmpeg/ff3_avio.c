/* ff3_avio.c -- [FF3] prove the right-way I/O: decode a frame THROUGH
 * libavformat's demuxer over a dos-backed custom AVIOContext (aros_avio).
 * Unlike FF1 (which fed raw bytes straight to the decoder), this exercises
 * avformat_open_input + av_read_frame -- the path real containers need.
 *
 *   FF3Avio <file> [resultfile]      (default MacRW:test.jpg)
 */
#include <proto/dos.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include "aros_avio.h"

static BPTR g_out = 0;
static void put(const char *s) { PutStr((CONST_STRPTR)s); if (g_out) FPuts(g_out, (CONST_STRPTR)s); }
static const char *udec(unsigned v, char *buf)
{
    char tmp[12]; int i = 0, j = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0'; return buf;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "MacRW:test.jpg";
    AVFormatContext *fmt = NULL;
    const AVCodec *dec = NULL;
    AVCodecContext *ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int vs, got = 0, npkt = 0, rc = 20;
    char b[12];

    if (argc > 2 && argv[2] && argv[2][0]) g_out = Open((CONST_STRPTR)argv[2], MODE_NEWFILE);
    put("[FF3] avio_open "); put(path); put("\n");

    fmt = aros_avio_open(path);
    if (!fmt) { put("[FF3] FAIL: aros_avio_open\n"); goto done; }
    put("[FF3] container "); put((fmt->iformat && fmt->iformat->name) ? fmt->iformat->name : "(?)"); put("\n");

    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (vs < 0 || !dec) { put("[FF3] FAIL: no video stream\n"); goto done; }
    ctx = avcodec_alloc_context3(dec);
    if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar) < 0
             || avcodec_open2(ctx, dec, NULL) < 0) { put("[FF3] FAIL: open codec\n"); goto done; }
    put("[FF3] codec "); put(dec->name ? dec->name : "(?)"); put("\n");

    pkt = av_packet_alloc(); frame = av_frame_alloc();
    if (!pkt || !frame) { put("[FF3] FAIL: alloc\n"); goto done; }

    while (!got) {
        int r = av_read_frame(fmt, pkt);
        if (r >= 0) {
            npkt++;
            if (pkt->stream_index == vs && avcodec_send_packet(ctx, pkt) == 0)
                if (avcodec_receive_frame(ctx, frame) == 0) got = 1;
            av_packet_unref(pkt);
        } else {
            avcodec_send_packet(ctx, NULL);
            if (avcodec_receive_frame(ctx, frame) == 0) got = 1;
            break;
        }
    }
    put("[FF3] packets="); put(udec((unsigned)npkt, b)); put("\n");
    if (!got) { put("[FF3] FAIL: no frame\n"); goto done; }

    put("[FF3] decoded "); put(udec(frame->width, b)); put("x"); put(udec(frame->height, b)); put("\n");
    put("FFMPEG-AROS: [FF3] AVIO DEMUX OK\n");
    rc = 0;

done:
    if (frame) av_frame_free(&frame);
    if (pkt)   av_packet_free(&pkt);
    if (ctx)   avcodec_free_context(&ctx);
    if (fmt)   aros_avio_close(fmt);
    if (rc) put("FFMPEG-AROS: [FF3] FAIL\n");
    if (g_out) Close(g_out);
    return rc;
}
