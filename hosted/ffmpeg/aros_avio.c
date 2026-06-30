/* aros_avio.c -- custom AVIOContext backed by AROS dos. See aros_avio.h. */
#include <proto/dos.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

#include "aros_avio.h"

#define AVIO_BUFSZ 32768

struct aros_io { BPTR fh; };

static int aros_read(void *opaque, uint8_t *buf, int size)
{
    struct aros_io *io = opaque;
    LONG n = Read(io->fh, buf, size);
    if (n < 0)  return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    return (int)n;
}

/* Classic 32-bit dos Seek(): moves the pointer and returns the position it held
 * BEFORE the move (-1 on error). 32-bit is fine for a media viewer (< 2GB). */
static int64_t aros_seek(void *opaque, int64_t off, int whence)
{
    struct aros_io *io = opaque;
    LONG mode;

    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) {
        LONG cur = Seek(io->fh, 0, OFFSET_CURRENT);     /* current pos, no move */
        LONG sz;
        Seek(io->fh, 0, OFFSET_END);                    /* go to end */
        sz = Seek(io->fh, cur, OFFSET_BEGINNING);       /* returns end (= size), restores pos */
        return (int64_t)sz;
    }
    mode = (whence == SEEK_SET) ? OFFSET_BEGINNING :
           (whence == SEEK_CUR) ? OFFSET_CURRENT  : OFFSET_END;
    if (Seek(io->fh, (LONG)off, mode) < 0) return AVERROR(EIO);
    return (int64_t)Seek(io->fh, 0, OFFSET_CURRENT);    /* the new absolute position */
}

AVFormatContext *aros_avio_open(const char *path)
{
    struct aros_io  *io;
    unsigned char   *buf;
    AVIOContext     *avio;
    AVFormatContext *fmt;

    io = av_mallocz(sizeof(*io));
    if (!io) return NULL;
    io->fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!io->fh) { av_free(io); return NULL; }

    buf = av_malloc(AVIO_BUFSZ);
    if (!buf) { Close(io->fh); av_free(io); return NULL; }

    avio = avio_alloc_context(buf, AVIO_BUFSZ, 0, io, aros_read, NULL, aros_seek);
    if (!avio) { av_free(buf); Close(io->fh); av_free(io); return NULL; }

    fmt = avformat_alloc_context();
    if (!fmt) {
        av_freep(&avio->buffer); avio_context_free(&avio);
        Close(io->fh); av_free(io);
        return NULL;
    }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        /* open_input freed fmt; the custom pb (CUSTOM_IO) is ours to free */
        av_freep(&avio->buffer); avio_context_free(&avio);
        Close(io->fh); av_free(io);
        return NULL;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        aros_avio_close(fmt);
        return NULL;
    }
    return fmt;
}

void aros_avio_close(AVFormatContext *fmt)
{
    AVIOContext    *avio;
    struct aros_io *io;

    if (!fmt) return;
    avio = fmt->pb;
    io   = avio ? avio->opaque : NULL;

    avformat_close_input(&fmt);          /* CUSTOM_IO: leaves pb for us */
    if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    if (io)   { if (io->fh) Close(io->fh); av_free(io); }
}
