/* Handwritten GadTools event crossings. Generated UI construction remains in
 * the OS-side table; these two vectors share the broker's guest queue and the
 * native IntuiMessage reply pairing. */
#include "emu68k_internal.h"
#include "emu68k_genlibs.h"
#include "bridge_lab.h"

#include <stdint.h>

/* Classic 32-bit IntuiMessage offsets.  The OS-side adapter builds this exact
 * facade; keeping the payload in the trace makes interactive failures about
 * Class/Code/identity visible instead of looking like an idle application. */
#define IMSG_CLASS       20u
#define IMSG_CODE        24u
#define IMSG_QUALIFIER   26u
#define IMSG_IADDRESS    28u
#define IMSG_MOUSEX      32u
#define IMSG_MOUSEY      34u
#define IMSG_WINDOW      44u

int emu68k_gadtools_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                         struct j5d_m68k_state *st, char *e, unsigned el)
{
    if (lvo == GADTOOLS_LVO_GT_GETIMSG) {
        uint32_t port = st->a[0];
        for (;;) {
            struct j5d_m68k_state get = *st;
            struct j5d_m68k_state filter;
            int rc;

            event_pump(r, st, port, 0, NULL);
            get.a[0] = port;
            rc = emu68k_exec_call(r, sb, LVO_GETMSG, &get, e, el);
            if (rc != 0 || !get.d[0]) {
                st->d[0] = get.d[0];
                return rc;
            }

            /* The OS-side GadTools adapter filters the native original paired
             * with this guest facade and copies the filtered view back. */
            filter = *st;
            filter.a[1] = get.d[0];
            if (!g_oscall ||
                g_oscall("gadtools.library", lvo, &filter, r->reserve,
                         g_oscall_user, e, el) != 0)
                return 1;
            if (filter.d[0]) {
                st->d[0] = filter.d[0];
                bl_event(BL_RUNTIME, r->cur_ctx, emu68k_ctx_task(r), st->pc,
                         "event.filter",
                         "\"kind\":\"gadtools\",\"message\":\"%s\","
                         "\"class\":%u,\"code\":%u,\"qualifier\":%u,"
                         "\"iaddress\":\"%s\",\"window\":\"%s\","
                         "\"mouse_x\":%d,\"mouse_y\":%d",
                         bl_id("message", filter.d[0]),
                         emu68k_gread32(sb, filter.d[0] + IMSG_CLASS),
                         emu68k_gread16(sb, filter.d[0] + IMSG_CODE),
                         emu68k_gread16(sb, filter.d[0] + IMSG_QUALIFIER),
                         bl_id("object", emu68k_gread32(sb,
                               filter.d[0] + IMSG_IADDRESS)),
                         bl_id("window", emu68k_gread32(sb,
                               filter.d[0] + IMSG_WINDOW)),
                         (int16_t)emu68k_gread16(sb,
                                  filter.d[0] + IMSG_MOUSEX),
                         (int16_t)emu68k_gread16(sb,
                                  filter.d[0] + IMSG_MOUSEY));
                return 0;
            }
            /* A GadTools-only message was consumed and replied natively. */
        }
    }
    if (lvo == GADTOOLS_LVO_GT_REPLYIMSG) {
        if (!st->a[1]) return 0;
        bl_event(BL_RUNTIME, r->cur_ctx, emu68k_ctx_task(r), st->pc,
                 "port.reply", "\"message\":\"%s\","
                 "\"adapter\":\"gadtools\"", bl_id("message", st->a[1]));
        if (g_oscall &&
            g_oscall("gadtools.library", lvo, st, r->reserve,
                     g_oscall_user, e, el) == 0)
            return 0;
        return 1;
    }
    return 1;
}
