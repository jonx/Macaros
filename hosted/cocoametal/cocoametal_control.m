/*
 * cocoametal_control.m — host-side control channel for driving AROS headlessly.
 *
 * When $AROS_CM_CONTROL names a path (a FIFO), cm__control_init opens it and a
 * dispatch source on the MAIN queue reads line commands. Keyboard/mouse commands
 * are turned into synthetic CMEvents that cm__pump_events_appkit drains FIRST —
 * i.e. they travel the exact same path real NSEvents do, into the AROS input
 * HIDD. A screenshot command reads back the rendered framebuffer to a PPM.
 *
 * This lets a process with no window-server session (e.g. an automation/CI
 * harness) puppet the running AROS: type, click, capture. It is also the seed of
 * the embeddable-library input/capture API — same protocol, callable in-process.
 *
 * Pulls no AROS headers; uses only the public cm_* ABI (cocoametal.h).
 *
 * Command grammar (one per line):
 *   K <vk> <pressed>     key event   (vk = macOS virtual keycode, pressed 1/0)
 *   M <x> <y>            mouse move  (logical, top-left)
 *   B <button> <pressed> mouse button(0=left 1=right 2=middle)
 *   W <dy> [dx]          scroll wheel: whole line steps, +dy = wheel down,
 *                        +dx = wheel right (CM_EV_WHEEL packing)
 *   R <w> <h>            resize host window content area (logical points)
 *   G <key> <value> [y]  AROS-facing setting event (CM_EV_SETTING)
 *   S <path>             screenshot the framebuffer to <path> (binary PPM)
 *   V start <path> [fps] [secs]  start recording to a .mov; if secs>0, auto-stop after secs
 *   V stop               finalize the recording
 */

#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cocoametal.h"

/* Implemented in cocoametal_window.m. This is an internal harness hook, not part
 * of the exported cm_* ABI. */
void cm__resize_window(CMContext *cx, int w, int h);

/* Implemented in cocoametal_shell.m (weak no-op in cocoametal.m for non-shell
 * builds). Schedules an auto-stop of the current recording after `seconds`. */
void cm__record_autostop(CMContext *cx, double seconds);

/* ---- injection ring (main-thread only: reader + drainer both run there) ---- */
#define CM_INJ_MAX 256
static CMEvent g_inj[CM_INJ_MAX];
static int     g_injHead, g_injTail;
static int     g_mouseX, g_mouseY;

static void cm__inj_push(const CMEvent *e) {
    int next = (g_injTail + 1) % CM_INJ_MAX;
    if (next == g_injHead) return;      /* full: drop (caller is faster than AROS) */
    g_inj[g_injTail] = *e;
    g_injTail = next;
}

/* Inject one synthetic key transition into the same ring the control FIFO uses,
 * so the host app shell (menu items) can hand keystrokes to AROS. Main thread:
 * the menu actions and the pump both run on the main queue. `vk` is a macOS
 * virtual keycode, `mods` a CM_MOD_* bitmask. */
void cm__inject_key(int vk, int pressed, unsigned mods) {
    CMEvent e;
    memset(&e, 0, sizeof e);
    e.type = CM_EV_KEY;
    e.code = vk;
    e.pressed = pressed;
    e.mods = mods;
    cm__inj_push(&e);
}

/* Drain queued injected events into out[]; returns count written. Main thread.
 * Declared (extern) and called at the top of cm__pump_events_appkit. */
int cm__control_drain(CMEvent *out, int maxEvents) {
    int n = 0;
    while (n < maxEvents && g_injHead != g_injTail) {
        out[n++] = g_inj[g_injHead];
        g_injHead = (g_injHead + 1) % CM_INJ_MAX;
    }
    return n;
}

/* ---- screenshot: framebuffer -> binary PPM (P6) via the public readback ---- */
static void cm__control_shot(CMContext *cx, const char *path) {
    int tw = 0, th = 0, scale = 0;
    if (cm_target_size(cx, &tw, &th, &scale) != 0 || scale <= 0) {
        NSLog(@"[cm-control] shot: target_size failed");
        return;
    }
    /* cm_readback wants the LOGICAL size (backing / scale). */
    int w = tw / scale, h = th / scale;
    if (w <= 0 || h <= 0)
        return;
    int stride = w * 4;
    unsigned char *buf = malloc((size_t)stride * h);
    if (!buf) return;
    if (cm_readback(cx, buf, stride, w, h) != 0) {
        NSLog(@"[cm-control] shot: readback failed");
        free(buf);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) {           /* BGRA -> RGB */
            unsigned char rgb[3] = { buf[i*4+2], buf[i*4+1], buf[i*4+0] };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        NSLog(@"[cm-control] shot %dx%d -> %s", w, h, path);
    }
    free(buf);
}

/* ---- command parsing ---- */
static void cm__control_exec(CMContext *cx, char *line) {
    CMEvent e;
    memset(&e, 0, sizeof e);
    switch (line[0]) {
    case 'K': {
        int vk, pressed, mods = 0;
        if (sscanf(line + 1, "%d %d %d", &vk, &pressed, &mods) >= 2) {
            e.type = CM_EV_KEY; e.code = vk; e.pressed = pressed;
            e.mods = (unsigned)mods;
            cm__inj_push(&e);
        }
        break;
    }
    case 'M': {
        int x, y;
        if (sscanf(line + 1, "%d %d", &x, &y) == 2) {
            e.type = CM_EV_MOUSEMOVE; e.x = x; e.y = y;
            g_mouseX = x; g_mouseY = y;
            cm__inj_push(&e);
        }
        break;
    }
    case 'B': {
        int b, pressed, x, y;
        int n = sscanf(line + 1, "%d %d %d %d", &b, &pressed, &x, &y);
        if (n >= 2) {
            e.type = CM_EV_MOUSEBTN; e.code = b; e.pressed = pressed;
            if (n >= 4) {
                g_mouseX = x; g_mouseY = y;
            }
            e.x = g_mouseX; e.y = g_mouseY;
            cm__inj_push(&e);
        }
        break;
    }
    case 'W': {   /* wheel: steps, +dy = wheel down, +dx = wheel right */
        int dy, dx = 0;
        if (sscanf(line + 1, "%d %d", &dy, &dx) >= 1) {
            e.type = CM_EV_WHEEL; e.x = dx; e.y = dy;
            cm__inj_push(&e);
        }
        break;
    }
    case 'R': {
        int w, h;
        if (sscanf(line + 1, "%d %d", &w, &h) == 2)
            cm__resize_window(cx, w, h);
        break;
    }
    case 'G': {
        int key, value, y = 0;
        if (sscanf(line + 1, "%d %d %d", &key, &value, &y) >= 2) {
            e.type = CM_EV_SETTING; e.code = key; e.x = value; e.y = y;
            cm__inj_push(&e);
        }
        break;
    }
    case 'S': {
        char path[1024];
        if (sscanf(line + 1, " %1023s", path) == 1)
            cm__control_shot(cx, path);
        break;
    }
    case 'V': {   /* movie recording: "V start <path> [fps] [seconds]" | "V stop" */
        char sub[16], path[1024]; int fps = 30; double secs = 0;
        int n = sscanf(line + 1, " %15s %1023s %d %lf", sub, path, &fps, &secs);
        if (n >= 1 && strcmp(sub, "stop") == 0) {
            NSLog(@"[cm-control] record stop (rc=%d)", cm_record_stop(cx));
        } else if (n >= 2 && strcmp(sub, "start") == 0) {
            if (fps <= 0) fps = 30;
            int rc = cm_record_start(cx, path, fps, 0);
            NSLog(@"[cm-control] record start %s @%dfps%s (rc=%d)",
                  path, fps, secs > 0 ? " (timed)" : "", rc);
            if (rc == 0 && secs > 0) cm__record_autostop(cx, secs);   /* auto-stop after N s */
        }
        break;
    }
    default:
        break;
    }
}

/* ---- FIFO reader on the main queue ---- */
static dispatch_source_t g_ctlSrc;
static char g_lineBuf[4096];
static int  g_lineLen;

void cm__control_init(CMContext *cx, const char *path) {
    if (!path || !*path)
        return;
    /* O_RDWR so the FIFO always has a writer end open here — the reader never
       sees EOF when an external writer closes after a one-shot `echo`. */
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        NSLog(@"[cm-control] open(%s) failed (errno %d)", path, errno);
        return;
    }
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0,
                                                   dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, ^{
        char tmp[1024];
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r <= 0)
            return;
        for (ssize_t i = 0; i < r; i++) {
            if (tmp[i] == '\n' || g_lineLen >= (int)sizeof(g_lineBuf) - 1) {
                g_lineBuf[g_lineLen] = 0;
                if (g_lineLen > 0)
                    cm__control_exec(cx, g_lineBuf);
                g_lineLen = 0;
            } else {
                g_lineBuf[g_lineLen++] = tmp[i];
            }
        }
    });
    dispatch_resume(src);
    g_ctlSrc = src;
    NSLog(@"[cm-control] listening on %s", path);
}
