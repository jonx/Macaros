/* Handwritten graphics.library guest-memory semantics.
 * Most graphics crossings are generated; exceptional callbacks/mirrors live here. */
#include "emu68k_internal.h"
#include "emu68k_genlibs.h"
#include "emu68k_guest_offsets.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int graphics_span(j4_sandbox *sb, uint32_t p, uint32_t n)
{
    return p >= sb->sandbox_origin &&
           (uint64_t)p + n <= (uint64_t)sb->sandbox_origin + sb->size;
}

int emu68k_graphics_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                         struct j5d_m68k_state *st, char *e, unsigned el)
{
    /* WaitTOF blocks the native AROS task until vertical blank.  The native
     * scheduler cannot see sibling 68k contexts inside this run, so give them
     * a cooperative turn before falling through to the generated native call.
     * A nested frame-loop context gets a bounded engine quantum and must not
     * recursively enter the parent that is waiting below it on the host stack.
     * Returning 1 with no error means "preflight done; continue generated". */
    if (lvo == GRAPHICS_LVO_WAITTOF) {
        if (r->ctx[r->cur_ctx].can_unwind)
            return 1;
        if (emu68k_reschedule_siblings(r, sb, "WaitTOF", st->pc, e, el) != 0)
            return 1;
        return 1;
    }

    switch (lvo) {
    case GRAPHICS_LVO_OPENFONT:
        /* The native implementation cannot dereference a NULL TextAttr, but
         * legacy callers use NULL as a probe and can handle OpenFont failure.
         * Preserve that guest-visible failure result instead of terminating
         * the entire translated program at the bridge boundary. */
        if (!st->a[0]) {
            st->d[0] = 0;
            return 0;
        }
        return 1;                       /* non-NULL: generated TextAttr copy */
    case GRAPHICS_LVO_SETCOLLISION: {
        uint32_t gi = st->a[1], table;
        if (st->d[0] >= 16u || !graphics_span(sb, gi, M68K_GelsInfo_SIZEOF))
            goto bad;
        table = emu68k_gread32(sb, gi + M68K_GelsInfo_collHandler);
        if (!graphics_span(sb, table, M68K_collTable_SIZEOF)) goto bad;
        emu68k_gwrite32(sb, table + M68K_collTable_collPtrs + st->d[0] * 4u,
                        st->a[0]);
        return 0;
    }
    case GRAPHICS_LVO_ADDANIMOB: {
        uint32_t object = st->a[0], keyp = st->a[1], old, comp;
        if (!graphics_span(sb, object, M68K_AnimOb_SIZEOF) ||
            !graphics_span(sb, keyp, 4)) goto bad;
        old = emu68k_gread32(sb, keyp);
        emu68k_gwrite32(sb, object + M68K_AnimOb_NextOb, old);
        emu68k_gwrite32(sb, object + M68K_AnimOb_PrevOb, 0);
        if (old) {
            if (!graphics_span(sb, old, M68K_AnimOb_SIZEOF)) goto bad;
            emu68k_gwrite32(sb, old + M68K_AnimOb_PrevOb, object);
        }
        emu68k_gwrite32(sb, keyp, object);
        comp = emu68k_gread32(sb, object + M68K_AnimOb_HeadComp);
        for (unsigned guard = 0; comp && guard < 4096u; guard++) {
            if (!graphics_span(sb, comp, M68K_AnimComp_SIZEOF)) goto bad;
            emu68k_gwrite16(sb, comp + M68K_AnimComp_Timer,
                emu68k_gread16(sb, comp + M68K_AnimComp_TimeSet));
            comp = emu68k_gread32(sb, comp + M68K_AnimComp_NextComp);
        }
        return 0;
    }
    case GRAPHICS_LVO_ANIMATE: {
        uint32_t keyp = st->a[0], object;
        if (!graphics_span(sb, keyp, 4)) goto bad;
        object = emu68k_gread32(sb, keyp);
        for (unsigned objects = 0; object && objects < 4096u; objects++) {
            uint32_t comp, routine;
            int32_t any, anx;
            int16_t yvel, xvel, yaccel, xaccel;
            if (!graphics_span(sb, object, M68K_AnimOb_SIZEOF)) goto bad;
            any = (int16_t)emu68k_gread16(sb, object + M68K_AnimOb_AnY);
            anx = (int16_t)emu68k_gread16(sb, object + M68K_AnimOb_AnX);
            emu68k_gwrite32(sb, object + M68K_AnimOb_Clock,
                emu68k_gread32(sb, object + M68K_AnimOb_Clock) + 1u);
            emu68k_gwrite16(sb, object + M68K_AnimOb_AnOldY, (uint16_t)any);
            emu68k_gwrite16(sb, object + M68K_AnimOb_AnOldX, (uint16_t)anx);
            yvel = (int16_t)emu68k_gread16(sb, object + M68K_AnimOb_YVel);
            xvel = (int16_t)emu68k_gread16(sb, object + M68K_AnimOb_XVel);
            yaccel = (int16_t)emu68k_gread16(sb, object + M68K_AnimOb_YAccel);
            xaccel = (int16_t)emu68k_gread16(sb, object + M68K_AnimOb_XAccel);
            any += yvel; anx += xvel;
            emu68k_gwrite16(sb, object + M68K_AnimOb_AnY, (uint16_t)any);
            emu68k_gwrite16(sb, object + M68K_AnimOb_AnX, (uint16_t)anx);
            emu68k_gwrite16(sb, object + M68K_AnimOb_YVel, (uint16_t)(yvel + yaccel));
            emu68k_gwrite16(sb, object + M68K_AnimOb_XVel, (uint16_t)(xvel + xaccel));
            routine = emu68k_gread32(sb, object + M68K_AnimOb_AnimORoutine);
            if (routine) {
                struct j5d_m68k_state call = *st;
                memset(&call, 0, sizeof call);
                if (emu68k_run_guest_subroutine(r, routine, &call, 0,
                                                NULL, e, el) != 0) return 1;
            }
            comp = emu68k_gread32(sb, object + M68K_AnimOb_HeadComp);
            for (unsigned components = 0; comp && components < 4096u; components++) {
                uint16_t timer;
                if (!graphics_span(sb, comp, M68K_AnimComp_SIZEOF)) goto bad;
                timer = emu68k_gread16(sb, comp + M68K_AnimComp_Timer);
                if (timer) timer--;
                emu68k_gwrite16(sb, comp + M68K_AnimComp_Timer, timer);
                routine = emu68k_gread32(sb, comp + M68K_AnimComp_AnimCRoutine);
                if (routine) {
                    struct j5d_m68k_state call = *st;
                    memset(&call, 0, sizeof call);
                    if (emu68k_run_guest_subroutine(r, routine, &call, 0,
                                                    NULL, e, el) != 0) return 1;
                }
                comp = emu68k_gread32(sb, comp + M68K_AnimComp_NextComp);
            }
            object = emu68k_gread32(sb, object + M68K_AnimOb_NextOb);
        }
        return 0;
    }
    case GRAPHICS_LVO_CMOVE: {
        uint32_t ucl = st->a[1], list, ins;
        if (!graphics_span(sb, ucl, M68K_UCopList_SIZEOF)) goto bad;
        list = emu68k_gread32(sb, ucl + M68K_UCopList_CopList);
        if (!graphics_span(sb, list, M68K_CopList_SIZEOF)) goto bad;
        ins = emu68k_gread32(sb, list + M68K_CopList_CopPtr);
        if (!graphics_span(sb, ins, M68K_CopIns_SIZEOF)) goto bad;
        emu68k_gwrite16(sb, ins + M68K_CopIns_OpCode, 0);
        /* The generated union offset names its pointer view; the MOVE view is
         * the same four bytes, split into destination register and data. */
        emu68k_gwrite16(sb, ins + M68K_CopIns_u3_nxtlist, st->d[0]);
        emu68k_gwrite16(sb, ins + M68K_CopIns_u3_nxtlist + 2u, st->d[1]);
        return 0;
    }
    case GRAPHICS_LVO_CHANGESPRITE:
        if (!graphics_span(sb, st->a[1], M68K_SimpleSprite_SIZEOF)) goto bad;
        emu68k_gwrite32(sb, st->a[1] + M68K_SimpleSprite_posctldata, st->a[2]);
        return 0;
    case GRAPHICS_LVO_ADDDISPLAYDRIVERA:
        st->d[0] = 4; /* DD_DRIVER_ERROR: guests cannot install native HIDDs */
        return 0;
    case GRAPHICS_LVO_SETDISPLAYDRIVERCALLBACK:
        return 0; /* private AROS callback; no guest-installed driver exists */
    case GRAPHICS_LVO_VIDEOCONTROL:
        st->d[0] = 1; /* documented bad/unsupported tag result */
        return 0;
    case GRAPHICS_LVO_DORENDERFUNC:
    case GRAPHICS_LVO_DOPIXELFUNC:
        /* These AROS-private entries expose native HIDD objects to callbacks;
         * a classic guest has no such object ABI. Zero is their documented
         * no-pixels-rendered result, allowing the caller's fallback path. */
        st->d[0] = 0;
        return 0;
    case GRAPHICS_LVO_INITAREA: {
        int32_t max = (int16_t)st->d[0];
        uint32_t bytes;
        if (max < 0 || (uint32_t)max > UINT32_MAX / 5u) goto bad;
        bytes = (uint32_t)max * 5u;
        if (!graphics_span(sb, st->a[0], M68K_AreaInfo_SIZEOF) ||
            !graphics_span(sb, st->a[1], bytes)) goto bad;
        emu68k_gwrite32(sb, st->a[0] + M68K_AreaInfo_VctrTbl, st->a[1]);
        emu68k_gwrite32(sb, st->a[0] + M68K_AreaInfo_VctrPtr, st->a[1]);
        emu68k_gwrite32(sb, st->a[0] + M68K_AreaInfo_FlagTbl,
                       st->a[1] + (uint32_t)max * 4u);
        emu68k_gwrite32(sb, st->a[0] + M68K_AreaInfo_FlagPtr,
                       st->a[1] + (uint32_t)max * 4u);
        emu68k_gwrite16(sb, st->a[0] + M68K_AreaInfo_Count, 0);
        emu68k_gwrite16(sb, st->a[0] + M68K_AreaInfo_MaxCount, (uint32_t)max);
        return 0;
    }
    case GRAPHICS_LVO_INITTMPRAS:
        if (!graphics_span(sb, st->a[0], M68K_TmpRas_SIZEOF) ||
            !graphics_span(sb, st->a[1], st->d[0])) goto bad;
        emu68k_gwrite32(sb, st->a[0] + M68K_TmpRas_RasPtr, st->a[1]);
        emu68k_gwrite32(sb, st->a[0] + M68K_TmpRas_Size, st->d[0]);
        st->d[0] = st->a[0];
        return 0;
    case GRAPHICS_LVO_BLTCLEAR: {
        uint32_t bytes = st->d[0];
        if (st->d[1] & 2u) bytes = (bytes & 0xffffu) * (bytes >> 16);
        bytes &= ~1u;
        if (!graphics_span(sb, st->a[1], bytes)) goto bad;
        memset(j4_sandbox_host(sb, st->a[1]), 0, bytes);
        return 0;
    }
    case GRAPHICS_LVO_ALLOCRASTER: {
        uint32_t row = ((((uint32_t)(uint16_t)st->d[0] + 15u) >> 3) & ~1u);
        uint32_t size = row * (uint32_t)(uint16_t)st->d[1];
        st->d[0] = size ? emu68k_guest_alloc(r, size) : 0;
        return 0;
    }
    case GRAPHICS_LVO_FREERASTER:
        return 0; /* the run-owned bump heap is reclaimed atomically */
    default:
        return 1;
    }
bad:
    if (e && el) snprintf(e, el, "graphics guest-memory argument is out of range");
    return 1;
}
