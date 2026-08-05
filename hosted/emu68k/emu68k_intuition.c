/* Handwritten intuition.library lifecycle that binds native IDCMP to guest ports. */
#include "emu68k_internal.h"
#include "emu68k_genlibs.h"
#include "emu68k_guest_offsets.h"
#include "bridge_lab.h"

#include <stdio.h>

static int intuition_span(j4_sandbox *sb, uint32_t p, uint32_t n)
{
    return p >= sb->sandbox_origin &&
           (uint64_t)p + n <= (uint64_t)sb->sandbox_origin + sb->size;
}

int emu68k_intuition_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                          struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case INTUITION_LVO_SETEDITHOOK:
        st->d[0] = r->intuition_edit_hook;
        r->intuition_edit_hook = st->a[0];
        return 0;
    case INTUITION_LVO_GADGETMOUSE:
        if (!intuition_span(sb, st->a[2], 4)) goto bad;
        emu68k_gwrite16(sb, st->a[2], 0);
        emu68k_gwrite16(sb, st->a[2] + 2u, 0);
        return 0;
    case INTUITION_LVO_NEXTOBJECT:
        if (!intuition_span(sb, st->a[0], 4)) goto bad;
        /* Private BOOPSI list nodes are not exposed by object facades. An
         * empty enumeration is the documented terminal result. */
        st->d[0] = 0;
        return 0;
    case INTUITION_LVO_DOGADGETMETHODA:
        st->d[0] = 0;
        return 0;
    case INTUITION_LVO_DONOTIFY:
        st->d[0] = 1;
        return 0;
    case INTUITION_LVO_STARTSCREENNOTIFYTAGLIST:
        st->d[0] = 0; /* registration failure: native events cannot target a guest task */
        return 0;
    case INTUITION_LVO_ENDSCREENNOTIFY:
        st->d[0] = 0xffffffffu;
        return 0;
    case INTUITION_LVO_AUTOREQUEST:
    case INTUITION_LVO_BUILDSYSREQUEST:
    case INTUITION_LVO_BUILDEASYREQUESTARGS:
    case INTUITION_LVO_SYSREQHANDLER:
        /* Hosted/headless policy: no process-global requester is opened on
         * behalf of an isolated guest.  Zero is the documented negative or
         * cancel/error answer for these entry points. */
        st->d[0] = 0;
        return 0;

    case INTUITION_LVO_ALLOCREMEMBER: {
        uint32_t keyp = st->a[0];
        uint32_t bytes = st->d[0];
        uint32_t memory, node;
        if (!intuition_span(sb, keyp, 4)) goto bad;
        memory = bytes ? emu68k_guest_alloc(r, bytes) : 0;
        node = emu68k_guest_alloc(r, M68K_Remember_SIZEOF);
        if (!node || (bytes && !memory)) {
            st->d[0] = 0;
            return 0;
        }
        emu68k_gwrite32(sb, node + M68K_Remember_NextRemember,
                        emu68k_gread32(sb, keyp));
        emu68k_gwrite32(sb, node + M68K_Remember_Memory, memory);
        emu68k_gwrite32(sb, node + M68K_Remember_RememberSize, bytes);
        emu68k_gwrite32(sb, keyp, node);
        st->d[0] = memory;
        return 0;
    }
    case INTUITION_LVO_FREEREMEMBER:
        if (!intuition_span(sb, st->a[0], 4)) goto bad;
        emu68k_gwrite32(sb, st->a[0], 0);
        return 0; /* all bump allocations are reclaimed with the run */

    default:
        break;
    }

    /* Guard rails in front of generated legacy-structure mirrors, not
     * handwritten implementations of these vectors. */
    if (lvo == INTUITION_LVO_OPENSCREEN) {
        if (!intuition_span(sb, st->a[0], M68K_NewScreen_SIZEOF)) goto bad;
        if (emu68k_gread16(sb, st->a[0] + M68K_NewScreen_Type) & 0x1000u) {
            if (e && el) snprintf(e, el,
                "OpenScreen NS_EXTENDED requires OpenScreenTagList translation");
            return 1;
        }
    }
    if (lvo == INTUITION_LVO_OPENWINDOW) {
        if (!intuition_span(sb, st->a[0], M68K_NewWindow_SIZEOF)) goto bad;
        if (emu68k_gread32(sb, st->a[0] + M68K_NewWindow_Flags) & (1u << 18)) {
            if (e && el) snprintf(e, el,
                "OpenWindow NW_EXTENDED requires OpenWindowTagList translation");
            return 1;
        }
    }

    if (lvo == INTUITION_LVO_MODIFYIDCMP) {
        uint32_t win = st->a[0];
        uint32_t port = emu68k_gread32(sb, win + M68K_Window_UserPort);
        if (port) {
            uint32_t bit = emu68k_gread8(sb, port + MP_SIGBIT);
            if (bit < 32)
                emu68k_event_bind(r, EMU68K_EVENT_IDCMP, win, port,
                                  1u << bit, "ModifyIDCMP", st->pc);
            bl_event(BL_RUNTIME, r->cur_ctx, emu68k_ctx_task(r), st->pc,
                     "port.bind",
                     "\"source\":\"native:idcmp:%s\",\"destination\":\"%s\","
                     "\"owner\":\"%s\",\"signal_bit\":%d,"
                     "\"reason\":\"ModifyIDCMP\"",
                     bl_id("window", win), bl_id("port", port),
                     bl_id("task", emu68k_gread32(sb, port + MP_SIGTASK)),
                     (int)bit);
            if (emu68k_trace_tasks())
                fprintf(stderr, "[68k/task] IDCMP bind window=%08x port=%08x "
                        "mp_SigTask=%08x mp_SigBit=%u (ctx=%d task=%08x)\n",
                        win, port, emu68k_gread32(sb, port + MP_SIGTASK),
                        (unsigned)bit, r->cur_ctx, emu68k_ctx_task(r));
        }
        /* The binding is bookkeeping before the native crossing, not the
         * implementation of ModifyIDCMP itself. */
        return 1;
    }
    (void)e; (void)el;
    return 1;
bad:
    if (e && el) snprintf(e, el, "intuition guest-memory argument is out of range");
    return 1;
}
