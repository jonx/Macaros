/* Handwritten layers.library guest-memory semantics.
 * Most layer crossings are generated; exceptional callbacks/mirrors live here. */
#include "emu68k_internal.h"
#include "emu68k_genlibs.h"
#include "emu68k_guest_offsets.h"

#include <stdio.h>
#include <string.h>

static int layers_span(j4_sandbox *sb, uint32_t p, uint32_t n)
{
    return p >= sb->sandbox_origin &&
           (uint64_t)p + n <= (uint64_t)sb->sandbox_origin + sb->size;
}

static uint32_t install_hook(uint32_t object, uint32_t hook,
                             void *storage, unsigned count)
{
    struct pair { uint32_t object, hook; } *pairs = storage;
    int free_slot = -1;
    for (unsigned i = 0; i < count; i++) {
        if (pairs[i].object == object) {
            uint32_t old = pairs[i].hook;
            pairs[i].hook = hook;
            return old;
        }
        if (!pairs[i].object && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot >= 0) {
        pairs[free_slot].object = object;
        pairs[free_slot].hook = hook;
    }
    return 0;
}

static int call_hook(struct emu68k_run *r, j4_sandbox *sb, uint32_t hook,
                     uint32_t object, uint32_t message,
                     struct j5d_m68k_state *basis, char *e, unsigned el)
{
    struct j5d_m68k_state call = *basis;
    uint32_t entry;
    if (hook <= 1u) return 0; /* LAYERS_NOBACKFILL/default sentinels */
    if (!layers_span(sb, hook, M68K_Hook_SIZEOF)) goto bad;
    entry = emu68k_gread32(sb, hook + M68K_Hook_h_Entry);
    if (!layers_span(sb, entry, 2)) goto bad;
    memset(&call, 0, sizeof call);
    call.a[0] = hook; call.a[1] = message; call.a[2] = object;
    return emu68k_run_guest_subroutine(r, entry, &call, 0, NULL, e, el);
bad:
    if (e && el) snprintf(e, el, "layers Hook or entry is outside guest memory");
    return 1;
}

int emu68k_layers_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                       struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case LAYERS_LVO_INSTALLLAYERHOOK:
        st->d[0] = install_hook(st->a[0], st->a[1], r->layer_hook,
            sizeof r->layer_hook / sizeof r->layer_hook[0]);
        return 0;
    case LAYERS_LVO_INSTALLLAYERINFOHOOK:
        st->d[0] = install_hook(st->a[0], st->a[1], r->layerinfo_hook,
            sizeof r->layerinfo_hook / sizeof r->layerinfo_hook[0]);
        return 0;
    case LAYERS_LVO_DOHOOKCLIPRECTS: {
        uint32_t rect = st->a[2], msg;
        if (!layers_span(sb, rect, M68K_Rectangle_SIZEOF)) goto bad;
        msg = emu68k_guest_alloc(r, 20);
        if (!msg) goto bad;
        emu68k_gwrite32(sb, msg, 0); /* layer is not dereferenced by classic backfills */
        emu68k_gwrite16(sb, msg + 4, emu68k_gread16(sb, rect + M68K_Rectangle_MinX));
        emu68k_gwrite16(sb, msg + 6, emu68k_gread16(sb, rect + M68K_Rectangle_MinY));
        emu68k_gwrite16(sb, msg + 8, emu68k_gread16(sb, rect + M68K_Rectangle_MaxX));
        emu68k_gwrite16(sb, msg + 10, emu68k_gread16(sb, rect + M68K_Rectangle_MaxY));
        emu68k_gwrite32(sb, msg + 12, (int16_t)emu68k_gread16(sb,
                                               rect + M68K_Rectangle_MinX));
        emu68k_gwrite32(sb, msg + 16, (int16_t)emu68k_gread16(sb,
                                               rect + M68K_Rectangle_MinY));
        return call_hook(r, sb, st->a[0], st->a[1], msg, st, e, el);
    }
    case LAYERS_LVO_COLLECTPIXELSLAYER: {
        uint32_t msg = emu68k_guest_alloc(r, 36);
        if (!msg) goto bad;
        memset(j4_sandbox_host(sb, msg), 0, 36);
        emu68k_gwrite32(sb, msg + 28, st->a[0]); /* cplm.layer */
        return call_hook(r, sb, st->a[2], st->a[0], msg, st, e, el);
    }
    default:
        return 1;
    }
bad:
    if (e && el) snprintf(e, el, "layers guest callback data is invalid");
    return 1;
}
