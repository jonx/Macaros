/* cocoametal_media.h — host media the user grants to AROS.
 *
 * The Mac owns the disks. This is where a person chooses which removable
 * medium the guest may see, and whether it may write to it. Granting unmounts
 * the volume on the macOS side (two writers on one filesystem corrupt it),
 * writes an AROS mount description into the shared directory, and records the
 * grant in aros-host.conf by the medium's identity, so a replug still resolves.
 *
 * The same two artifacts are written by graft/macaros-media (the command-line
 * half), and MediaWatch inside AROS mounts what appears. See
 * docs/features/host-media/README.md.
 */
#ifndef COCOAMETAL_MEDIA_H
#define COCOAMETAL_MEDIA_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CM_MEDIA_NONE      = 0,   /* AROS cannot see it        */
    CM_MEDIA_READONLY  = 1,   /* AROS may read it          */
    CM_MEDIA_READWRITE = 2    /* AROS may read and write   */
};

typedef struct {
    char               bsd[32];    /* "disk4s1"                              */
    char               label[64];  /* volume name, "-" when unnamed          */
    char               fs[16];     /* "exfat", "msdos", "apfs", ...          */
    char               aros[16];   /* AROS device name once granted          */
    unsigned long long size;       /* bytes                                  */
    int                mounted;    /* macOS has it mounted right now         */
    int                writable;   /* the medium itself accepts writes       */
    int                supported;  /* AROS has a handler for this filesystem */
    int                grant;      /* CM_MEDIA_*                             */
} CMMediaItem;

/* Snapshot of the removable media attached right now, never the host's own
 * disk. Returns the number written to `items` (at most `max`). */
int cm_media_scan(CMMediaItem *items, int max);

/* Grant or withdraw one medium. Unmounts it on the macOS side when granting and
 * hands it back to macOS when withdrawing. Returns 0, else fills `err`. */
int cm_media_set_grant(const char *bsd, int grant, char *err, int errlen);

/* Re-resolve every recorded grant against what is attached now and refresh its
 * mount description. Called once as the display comes up, because a /dev/disk
 * number is reassigned on every replug. Returns the number prepared. */
int cm_media_prepare(void);

/* Call `cb` whenever a medium appears or disappears. Schedules the notification
 * session on the current run loop; pass NULL to stop. */
void cm_media_watch(void (*cb)(void *ctx), void *ctx);

/* The shared directory holding the mount descriptions (host-side path). */
const char *cm_media_dir(void);

#ifdef __cplusplus
}
#endif
#endif /* COCOAMETAL_MEDIA_H */
