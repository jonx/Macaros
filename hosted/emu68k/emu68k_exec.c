/* exec.library guest semantics for the hosted 68k bridge. */
#include "emu68k_internal.h"
#include "emu68k_genlibs.h"
#include "emu68k_guest_offsets.h"
#include "bridge_lab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#define exec_call emu68k_exec_call

static int guest_span_ok(j4_sandbox *sb, uint32_t addr, uint32_t size)
{
    return addr >= sb->sandbox_origin &&
           (uint64_t)addr + size <= (uint64_t)sb->sandbox_origin + sb->size;
}

/* tc_TrapAlloc shares a union with tc_ETask.  Processes and other extended
 * tasks carry TF_ETASK and keep the allocation word in the pointed-to ETask;
 * plain Tasks keep the legacy inline word.  Treating the union bytes as the
 * inline mask corrupts the ETask pointer and makes AllocTrap depend on its
 * address. */
static uint32_t guest_trapalloc_addr(j4_sandbox *sb, uint32_t task,
                                     char *e, unsigned el)
{
    uint32_t addr = task + TASK_TRAPALLOC_OFF;
    if (gread8(sb, task + M68K_Task_tc_Flags) & TF_ETASK_GUEST) {
        uint32_t etask = gread32(sb, task + TASK_ETASK_OFF);
        if (!guest_span_ok(sb, etask, M68K_ETask_SIZEOF)) {
            snprintf(e, el, "Task %08x has an invalid ETask %08x",
                     task, etask);
            return 0;
        }
        addr = etask + ETASK_TRAPALLOC_OFF;
    }
    return addr;
}

static int port_has_event_kind(const struct emu68k_run *r, uint32_t port,
                               unsigned kind)
{
    int i;
    for (i = 0; i < EMU68K_EVENT_MAX; i++)
        if (r->event_source[i].live && r->event_source[i].port == port &&
            r->event_source[i].kind == kind)
            return 1;
    return 0;
}

static uint32_t avl_extreme(j4_sandbox *sb, uint32_t node, unsigned direction,
                            char *e, unsigned el)
{
    unsigned guard = 0;
    while (node) {
        uint32_t next;
        if (!guest_span_ok(sb, node, 16u)) {
            snprintf(e, el, "AVL node %08x is outside guest memory", node);
            return UINT32_MAX;
        }
        next = gread32(sb, node + (direction ? AVL_RIGHT_OFF : AVL_LEFT_OFF));
        if (!next) return node;
        node = next;
        if (++guard > 65536u) {
            snprintf(e, el, "AVL tree exceeds 65536 nodes or contains a cycle");
            return UINT32_MAX;
        }
    }
    return 0;
}

static uint32_t avl_adjacent(j4_sandbox *sb, uint32_t node, unsigned next,
                             char *e, unsigned el)
{
    uint32_t branch, parent, child;
    unsigned guard = 0;
    if (!node) return 0;
    if (!guest_span_ok(sb, node, 16u)) {
        snprintf(e, el, "AVL node %08x is outside guest memory", node);
        return UINT32_MAX;
    }
    branch = gread32(sb, node + (next ? AVL_RIGHT_OFF : AVL_LEFT_OFF));
    if (branch)
        return avl_extreme(sb, branch, next ? 0u : 1u, e, el);
    child = node;
    parent = gread32(sb, node + AVL_PARENT_OFF);
    while (parent) {
        uint32_t link;
        if (!guest_span_ok(sb, parent, 16u)) {
            snprintf(e, el, "AVL parent %08x is outside guest memory", parent);
            return UINT32_MAX;
        }
        link = gread32(sb, parent + (next ? AVL_LEFT_OFF : AVL_RIGHT_OFF));
        if (link == child) return parent;
        child = parent;
        parent = gread32(sb, parent + AVL_PARENT_OFF);
        if (++guard > 65536u) {
            snprintf(e, el, "AVL parent chain exceeds 65536 nodes or contains a cycle");
            return UINT32_MAX;
        }
    }
    return 0;
}

static int avl_compare_key(struct emu68k_run *r, uint32_t callback,
                           uint32_t node, uint32_t key, int32_t *comparison,
                           char *e, unsigned el)
{
    struct j5d_m68k_state call;
    uint32_t result = 0;
    if (!callback) {
        snprintf(e, el, "AVL key comparison callback is NULL");
        return 1;
    }
    memset(&call, 0, sizeof call);
    call.a[0] = node;
    call.a[1] = key;
    call.a[6] = EXEC_BASE;
    if (run_guest_subroutine(r, callback, &call, 0, &result, e, el) != 0)
        return 1;
    *comparison = (int32_t)result;
    return 0;
}

static int run_guest_interrupt(struct emu68k_run *r, j4_sandbox *sb,
                               uint32_t interrupt, uint32_t mask,
                               char *e, unsigned el)
{
    struct j5d_m68k_state call;
    uint32_t code;
    if (!guest_span_ok(sb, interrupt, M68K_Interrupt_SIZEOF)) {
        snprintf(e, el, "Interrupt %08x is outside guest memory", interrupt);
        return 1;
    }
    code = gread32(sb, interrupt + INTR_CODE_OFF);
    if (!code) {
        snprintf(e, el, "Interrupt %08x has a NULL is_Code", interrupt);
        return 1;
    }
    memset(&call, 0, sizeof call);
    call.a[0] = 0x00dff000u;              /* classic Custom base convention */
    call.a[1] = gread32(sb, interrupt + INTR_DATA_OFF);
    call.a[5] = code;
    call.a[6] = EXEC_BASE;
    call.d[1] = mask;
    return run_guest_subroutine(r, code, &call, 0, NULL, e, el);
}

static int avl_collect(j4_sandbox *sb, uint32_t root, uint32_t **out,
                       size_t *count, char *e, unsigned el)
{
    uint32_t *nodes = NULL, node;
    size_t n = 0, cap = 0;
    node = avl_extreme(sb, root, 0, e, el);
    if (node == UINT32_MAX) return 1;
    while (node) {
        uint32_t next;
        if (n == cap) {
            size_t newcap = cap ? cap * 2u : 32u;
            uint32_t *grown;
            if (newcap > 65536u) {
                free(nodes);
                snprintf(e, el, "AVL tree exceeds 65536 nodes");
                return 1;
            }
            grown = realloc(nodes, newcap * sizeof *nodes);
            if (!grown) {
                free(nodes);
                snprintf(e, el, "host memory exhausted walking AVL tree");
                return 1;
            }
            nodes = grown;
            cap = newcap;
        }
        nodes[n++] = node;
        next = avl_adjacent(sb, node, 1, e, el);
        if (next == UINT32_MAX) { free(nodes); return 1; }
        if (next == node) {
            free(nodes);
            snprintf(e, el, "AVL traversal contains a self-cycle");
            return 1;
        }
        node = next;
    }
    *out = nodes;
    *count = n;
    return 0;
}

static uint32_t avl_rebuild_range(j4_sandbox *sb, uint32_t *nodes,
                                  size_t lo, size_t hi, uint32_t parent,
                                  int *height)
{
    size_t mid;
    uint32_t node, left, right;
    int lh = 0, rh = 0;
    if (lo >= hi) { *height = 0; return 0; }
    mid = lo + (hi - lo) / 2u;
    node = nodes[mid];
    left = avl_rebuild_range(sb, nodes, lo, mid, node, &lh);
    right = avl_rebuild_range(sb, nodes, mid + 1u, hi, node, &rh);
    gwrite32(sb, node + AVL_LEFT_OFF, left);
    gwrite32(sb, node + AVL_RIGHT_OFF, right);
    gwrite32(sb, node + AVL_PARENT_OFF, parent);
    gwrite32(sb, node + 12u, (uint32_t)(int32_t)(rh - lh));
    *height = (lh > rh ? lh : rh) + 1;
    return node;
}

static void avl_rebuild(j4_sandbox *sb, uint32_t rootp,
                        uint32_t *nodes, size_t count)
{
    int height;
    gwrite32(sb, rootp, avl_rebuild_range(sb, nodes, 0, count, 0, &height));
}

int emu68k_exec_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                     struct j5d_m68k_state *st, char *e, unsigned el)
{
    switch (lvo) {
    case LVO_GL_INIT_DONE:
        return guestlib_init_done(r, st, e, el);
    case LVO_GL_OPEN_DONE:
        return guestlib_open_done(r, st, e, el);
    case LVO_GL_CLOSE_DONE:
        return guestlib_close_done(r, st, e, el);
    case LVO_GL_RECLAIM:
        return guestlib_reclaim(r, st, e, el);

    case LVO_RESCHEDULE: {
        /* Cooperative guest contexts normally switch when Wait blocks. An
         * explicit Reschedule gives every runnable sibling one turn without
         * involving the native scheduler (whose tasks are a different world). */
        return emu68k_reschedule_siblings(r, sb, "Reschedule", st->pc, e, el);
    }
    case LVO_EXCEPTION: {
        uint32_t task = ctx_task(r);
        unsigned guard = 0;
        gwrite8(sb, task + M68K_Task_tc_Flags,
                gread8(sb, task + M68K_Task_tc_Flags) & ~TF_EXCEPT_GUEST);
        for (;;) {
            uint32_t excepts = gread32(sb, task + TASK_SIGEXCEPT_OFF);
            uint32_t received = gread32(sb, task + TASK_SIGRECVD_OFF);
            uint32_t flags = excepts & received;
            uint32_t code, returned = 0;
            struct j5d_m68k_state call;
            if (!flags) return 0;
            gwrite32(sb, task + TASK_SIGEXCEPT_OFF, excepts ^ flags);
            gwrite32(sb, task + TASK_SIGRECVD_OFF, received ^ flags);
            code = gread32(sb, task + TASK_EXCEPTCODE_OFF);
            if (code) {
                memset(&call, 0, sizeof call);
                call.d[0] = flags;
                call.a[1] = gread32(sb, task + TASK_EXCEPTDATA_OFF);
                call.a[6] = EXEC_BASE;
                if (run_guest_subroutine(r, code, &call, 0, &returned, e, el) != 0)
                    return 1;
                gwrite32(sb, task + TASK_SIGEXCEPT_OFF,
                         gread32(sb, task + TASK_SIGEXCEPT_OFF) | returned);
            }
            if (++guard > 64u) {
                snprintf(e, el, "capability gap: exec.library.Exception callback "
                                "keeps rearming pending signals");
                return 1;
            }
        }
    }
    case LVO_FINDRESIDENT:
        /* Guest-side libraries are registered by the hunk loader, not exposed
         * as ROM Resident nodes. There is therefore no resident namespace. */
        st->d[0] = 0;
        return 0;
    case LVO_INITRESIDENT:
        /* Consistent with FindResident above: no guest Resident can be handed
         * here, and native Resident pointers must never enter guest memory. */
        st->d[0] = 0;
        return 0;
    case LVO_SETSR:
        /* AROS itself specifies ~0 when SetSR is unavailable off 68k. */
        st->d[0] = UINT32_MAX;
        return 0;
    case LVO_SETINTVECTOR: {
        uint32_t level = st->d[0], interrupt = st->a[1], old;
        if (level >= 32u ||
            (interrupt && !guest_span_ok(sb, interrupt, M68K_Interrupt_SIZEOF))) {
            snprintf(e, el, "SetIntVector level or Interrupt is invalid");
            return 1;
        }
        old = r->int_vector[level];
        r->int_vector[level] = interrupt;
        st->d[0] = old;
        return 0;
    }
    case LVO_ADDINTSERVER: {
        uint32_t level = st->d[0], interrupt = st->a[1];
        if (level >= 32u || !guest_span_ok(sb, interrupt, M68K_Interrupt_SIZEOF)) {
            snprintf(e, el, "AddIntServer level or Interrupt is invalid");
            return 1;
        }
        for (int i = 0; i < r->nintserver; i++)
            if (r->int_server[i].level == level &&
                r->int_server[i].interrupt == interrupt) return 0;
        if (r->nintserver >= (int)(sizeof r->int_server / sizeof r->int_server[0])) {
            snprintf(e, el, "more guest interrupt servers than this run keeps");
            return 1;
        }
        r->int_server[r->nintserver].level = level;
        r->int_server[r->nintserver].interrupt = interrupt;
        r->nintserver++;
        return 0;
    }
    case LVO_REMINTSERVER: {
        uint32_t level = st->d[0], interrupt = st->a[1];
        for (int i = 0; i < r->nintserver; i++)
            if (r->int_server[i].level == level &&
                r->int_server[i].interrupt == interrupt) {
                memmove(&r->int_server[i], &r->int_server[i + 1],
                        (size_t)(r->nintserver - i - 1) * sizeof r->int_server[0]);
                r->nintserver--;
                break;
            }
        return 0;
    }
    case LVO_CAUSE:
        /* Cause is a software interrupt. Run it immediately at this
         * cooperative boundary; no native interrupt state is involved. */
        return run_guest_interrupt(r, sb, st->a[1], 0, e, el);
    case LVO_SETEXCEPT: {
        uint32_t task = ctx_task(r), mask = st->d[1];
        uint32_t old = gread32(sb, task + TASK_SIGEXCEPT_OFF);
        uint32_t now = (old & ~mask) | (st->d[0] & mask);
        int rc = 0;
        gwrite32(sb, task + TASK_SIGEXCEPT_OFF, now);
        if (now & gread32(sb, task + TASK_SIGRECVD_OFF)) {
            gwrite8(sb, task + M68K_Task_tc_Flags,
                    gread8(sb, task + M68K_Task_tc_Flags) | TF_EXCEPT_GUEST);
            rc = exec_call(r, sb, LVO_EXCEPTION, st, e, el);
        }
        st->d[0] = old;
        return rc;
    }

    case LVO_RAWDOFMT:
        /* Not served here: RawDoFmt calls the PROGRAM's PutChProc once per
         * character, so doing it natively would mean re-entering the JIT from
         * inside a native call, and its argument block cannot be converted
         * without parsing the format string first (a %s argument is a guest
         * pointer, a %d argument is not). Redirect into 68k code instead and
         * all of it stays inside the guest address space. */
        st->pc = OSCODE_RAWDOFMT;
        return J5D_LVO_REDIRECT;

    case LVO_TAGGEDOPENLIBRARY: {
        static const char *const names[] = {
            "graphics.library", "layers.library", "intuition.library",
            "dos.library", "icon.library", "expansion.library",
            "utility.library", "keymap.library", "gadtools.library",
            "workbench.library"
        };
        static const char *const notices[] = {
            "AROS Research Operating System (AROS)",
            "Copyright (c) 1995-2026, ", "The AROS Development Team.",
            "Other parts (c) by respective owners.", "ALPHA ",
            "AROS hosted 68k bridge", "exec 51.8\r\n"
        };
        int32_t tag = (int32_t)st->d[0];
        const char *text = NULL;
        uint32_t guest;
        if (tag > 0 && (unsigned)tag <= sizeof names / sizeof names[0]) {
            text = names[tag - 1];
            guest = guest_strdup(r, text, strlen(text));
            if (!guest) {
                snprintf(e, el, "guest memory exhausted for TaggedOpenLibrary");
                return 1;
            }
            st->a[1] = guest;
            st->d[0] = 0;
            return exec_call(r, sb, LVO_OPENLIBRARY, st, e, el);
        }
        if (tag < 0 && (unsigned)(-tag) <= sizeof notices / sizeof notices[0])
            text = notices[-tag - 1];
        if (!text) {
            st->d[0] = 0;
            return 0;
        }
        guest = guest_strdup(r, text, strlen(text));
        if (!guest) {
            snprintf(e, el, "guest memory exhausted for TaggedOpenLibrary text");
            return 1;
        }
        st->d[0] = guest;
        return 0;
    }

    case LVO_OLDOPENLIBRARY:      /* same thing, older entry point: A1 = name     */
    case LVO_OPENLIBRARY: {
        const char *nm = guest_cstr(sb, st->a[1]);      /* A1 = name, D0 = ver  */
        uint32_t requested = (lvo == LVO_OLDOPENLIBRARY) ? 0u : st->d[0];
        int gi;
        if (!nm) {
            snprintf(e, el, "OpenLibrary: name pointer A1=%08x is outside the "
                     "guest arena %08x..%08x", st->a[1], sb->sandbox_origin,
                     sb->sandbox_origin + sb->size);
            return 1;
        }
        if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
            fprintf(stderr, "[68k] OpenLibrary(\"%s\", %u)\n", nm, requested);
        for (int i = 0; i < r->nlib; i++)                 /* already open?       */
            if (!strcmp(r->openlib[i].name, nm)) { st->d[0] = r->openlib[i].base; return 0; }

        gi = find_guestlib_name(r, nm);
        if (gi >= 0) {
            struct guestlib_live *g = &r->guestlib[gi];
            if (g->state == GL_READY && g->resident.version >= requested) {
                guestlib_save_preserved(g, st);
                st->pc = g->open_trampoline;
                return J5D_LVO_REDIRECT;
            }
            /* LOADING/OPENING is an explicit dependency cycle. FAILED and an
             * unsatisfied version are ordinary OpenLibrary failure too. */
            st->d[0] = 0;
            return 0;
        }

        /* A native AROS process enters with stdc/stdcio's task-storage bases
         * already established.  A new 68k execution domain cannot inherit
         * those 64-bit pointers, so bootstrap the genuine guest runtimes before
         * posixc's Init set asks for them.  This is library-order policy; all
         * three initializers and opens still execute as 68k guest code. */
        {
            const char *leaf = strrchr(nm, ':');
            const char *slash = strrchr(nm, '/');
            char depwhy[256] = {0};
            if (!leaf || (slash && slash > leaf)) leaf = slash;
            leaf = leaf ? leaf + 1 : nm;
            if (!strcmp(leaf, "posixc.library") &&
                (open_guestlib_now(r, "stdc.library", 0, NULL,
                                   depwhy, sizeof depwhy) ||
                 open_guestlib_now(r, "stdcio.library", 0, NULL,
                                   depwhy, sizeof depwhy))) {
                if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
                    fprintf(stderr, "[68k] posixc guest dependency failed: %s\n",
                            depwhy);
                st->d[0] = 0;
                return 0;
            }
        }

        {
            /* Exactly the libraries the bridge generated crossings for. Kept
             * in step by generating it: offering a name with nothing behind it
             * turns the program's first call into a capability gap, and
             * withholding one we did generate sends it looking on disk for a
             * 68k library that is not there. */
#define EMU_SERVABLE_ROW(name) name,
            static const char *const servable[] = {
                EMU68K_SERVABLE_LIBS(EMU_SERVABLE_ROW)
            };
#undef EMU_SERVABLE_ROW
            /* Match on the FILE NAME, because "LIBS:diskfont.library" is an
             * ordinary way to ask for the system library and an exact compare
             * sends it to the guest loader to look for a 68k file that is not
             * there. The guest search above has already had its turn, so a
             * program that ships its own library still gets that one. */
            const char *leaf = nm;
            const char *sep;
            for (sep = nm; *sep; sep++)
                if (*sep == '/' || *sep == ':') leaf = sep + 1;
            unsigned k; int known = 0;
            for (k = 0; k < sizeof servable / sizeof servable[0]; k++)
                if (!strcmp(leaf, servable[k])) { known = 1; break; }
            if (known && route_guestside(leaf)) {
                if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
                    fprintf(stderr, "[68k] OpenLibrary %s routed guest-side\n",
                            leaf);
                known = 0;
            }
            if (known && g_oscall) {
                uint32_t base = emu68k_native_facade_base(r, leaf, e, el);
                if (!base) return 1;
                {   /* give the handed-out base a version, for the same reason */
                    uint8_t *lb = j4_sandbox_host(sb, base);
                    lb[LIB_VERSION_OFF]      = (uint8_t)(GUEST_LIB_VERSION >> 8);
                    lb[LIB_VERSION_OFF + 1]  = (uint8_t)GUEST_LIB_VERSION;
                    lb[LIB_REVISION_OFF]     = (uint8_t)(GUEST_LIB_REV >> 8);
                    lb[LIB_REVISION_OFF + 1] = (uint8_t)GUEST_LIB_REV;
                }
                st->d[0] = base;
                return 0;
            }
        }

        {   /* Native-name-wins above; an unknown name is now a disk library. */
            char why[256] = {0};
            if (load_guestlib(r, nm, requested, &gi, why, sizeof why)) {
                if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
                    fprintf(stderr, "[68k] OpenLibrary guest %s failed: %s\n", nm, why);
                st->d[0] = 0;
                return 0;
            }
            struct guestlib_live *g = &r->guestlib[gi];
            guestlib_save_preserved(g, st);
            st->d[0] = (g->resident.flags & GL68_RTF_AUTOINIT) ? g->init.base : 0;
            st->a[0] = g->init.seglist;
            st->a[4] = 0;
            st->a[6] = EXEC_BASE;
            st->pc = g->init_trampoline;
            return J5D_LVO_REDIRECT;
        }
    }
    case LVO_CLOSELIBRARY: {
        int gi = find_guestlib_base(r, st->a[1]);       /* A1 = library base */
        if (gi >= 0) {
            guestlib_save_preserved(&r->guestlib[gi], st);
            /* Close runs on the base the caller was given, not on the
             * library's own: that is the only way a per-opener base can free
             * the right instance. */
            r->guestlib[gi].closing_base = st->a[1];
            st->a[6] = st->a[1];
            st->pc = r->guestlib[gi].close_trampoline;
            return J5D_LVO_REDIRECT;
        }
        st->d[0] = 0;
        return 0;                                        /* native facade stays */
    }
    case LVO_AVAILMEM:
        st->d[0] = (r->exec_heap_end > r->exec_heap)
                 ? (r->exec_heap_end - r->exec_heap) : 0;
        return 0;
    case LVO_FREEVEC:
        st->d[0] = 0;                                    /* bump heap: no free  */
        return 0;
    case LVO_CREATEPOOL: {
        /* PoolHeader is opaque to callers. Give it stable guest identity so
         * accidental field reads remain in-range, while the allocations it
         * produces come from the same guest heap as AllocMem/AllocVec. */
        for (int i = 0; i < GUESTPOOL_MAX; i++) {
            if (r->guestpool[i].live) continue;
            uint32_t token = guest_alloc(r, 32u);
            if (!token) { st->d[0] = 0; return 0; }
            r->guestpool[i].live = 1;
            r->guestpool[i].guest = token;
            r->guestpool[i].requirements = st->d[0];
            st->d[0] = token;
            return 0;
        }
        st->d[0] = 0;
        return 0;
    }
    case LVO_DELETEPOOL:
        for (int i = 0; i < GUESTPOOL_MAX; i++)
            if (r->guestpool[i].live && r->guestpool[i].guest == st->a[0]) {
                memset(&r->guestpool[i], 0, sizeof r->guestpool[i]);
                st->d[0] = 0;
                return 0;
            }
        snprintf(e, el, "DeletePool received unknown guest pool %08x", st->a[0]);
        return 1;
    case LVO_ALLOCPOOLED:
        for (int i = 0; i < GUESTPOOL_MAX; i++)
            if (r->guestpool[i].live && r->guestpool[i].guest == st->a[0]) {
                st->d[0] = guest_alloc(r, st->d[0]);
                return 0;
            }
        snprintf(e, el, "AllocPooled received unknown guest pool %08x", st->a[0]);
        return 1;
    case LVO_FREEPOOLED:
        for (int i = 0; i < GUESTPOOL_MAX; i++)
            if (r->guestpool[i].live && r->guestpool[i].guest == st->a[0]) {
                st->d[0] = 0;                           /* bump heap: no free */
                return 0;
            }
        snprintf(e, el, "FreePooled received unknown guest pool %08x", st->a[0]);
        return 1;
    case LVO_ALLOCATE:       /* Allocate(MemHeader A0, size D0): the header is
                              * the guest's idea of where memory comes from; in
                              * this arena there is one place, so serve it. */
    case LVO_ALLOCVEC:       /* the single most-called allocation in real code */
    case LVO_ALLOCMEM: {
        /* Memory a 68k program allocates must live in the GUEST arena: the
         * program dereferences the pointer itself, so it has to be an address
         * the program can reach. Real software asks for real amounts (DMS wants
         * hundreds of KB before it will even start), so this comes from a large
         * region sized to the arena, not from the corpus stub's small heap. */
        uint32_t size = (st->d[0] + 7u) & ~7u;
        if (size == 0 || r->exec_heap + size > r->exec_heap_end) {
            st->d[0] = 0;                                /* AmigaOS: NULL       */
            return 0;
        }
        st->d[0] = r->exec_heap;
        r->exec_heap += size;
        if (r->sb.next_alloc < r->exec_heap) r->sb.next_alloc = r->exec_heap;
        memset(j4_sandbox_host(sb, st->d[0]), 0, size);  /* MEMF_CLEAR-safe     */
        return 0;
    }
    case LVO_ALLOCABS: {
        uint32_t size = (st->d[0] + 7u) & ~7u;
        uint32_t where = st->a[1] & ~7u;
        if (!size || where < r->exec_heap ||
            (uint64_t)where + size > r->exec_heap_end ||
            !guest_span_ok(sb, where, size)) {
            st->d[0] = 0;
            return 0;
        }
        /* The allocator is monotonic. Advancing across the requested block may
         * waste the preceding gap, but guarantees no later allocation can
         * overlap the fixed address. Allocation failure remains legal; overlap
         * never is. */
        r->exec_heap = where + size;
        if (r->sb.next_alloc < r->exec_heap) r->sb.next_alloc = r->exec_heap;
        memset(j4_sandbox_host(sb, where), 0, size);
        st->d[0] = where;
        return 0;
    }
    case LVO_FREEMEM:
        st->d[0] = 0;                                    /* bump heap: no free  */
        return 0;

    /* Forbid/Permit control switching between the guest contexts kept by the
     * bridge.  Native AROS cannot see those contexts, so its scheduler cannot
     * enforce this for us.  In particular, yielding halfway through a child
     * publishing a MsgPort lets its parent queue work that the child then
     * erases when it finishes NewList(). */
    case LVO_FORBID:
        if (r->nctx && r->cur_ctx >= 0 && r->cur_ctx < r->nctx &&
            r->ctx[r->cur_ctx].forbid_depth != UINT16_MAX)
            r->ctx[r->cur_ctx].forbid_depth++;
        return 0;
    case LVO_PERMIT:
        if (r->nctx && r->cur_ctx >= 0 && r->cur_ctx < r->nctx &&
            r->ctx[r->cur_ctx].forbid_depth)
            r->ctx[r->cur_ctx].forbid_depth--;
        return 0;
    /* Interrupt exclusion remains bookkeeping: guest code does not run from
     * a native interrupt and every arena access stays on this host thread. */
    case LVO_DISABLE: case LVO_ENABLE:
        return 0;
    case LVO_FINDTASK:
        /* FindTask(NULL) = "me": the guest Process, which exists so that reading
         * pr_CLI says "launched from the Shell". */
        st->d[0] = (st->a[1] == 0) ? ctx_task(r) : 0;
        return 0;
    case LVO_ADDTASK:
        return add_guest_task_context(r, sb, st->a[1], st->a[2], st->a[3],
                                      0, st, e, el);
    case LVO_NEWADDTASK:
        return add_guest_task_context(r, sb, st->a[1], st->a[2], st->a[3],
                                      st->a[4], st, e, el);
    case LVO_NEWCREATETASKA:
        return create_guest_task(r, sb, st->a[0], st, e, el);
    case LVO_REMTASK: {
        uint32_t task = st->a[1] ? st->a[1] : ctx_task(r);
        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                 "task.remove", "\"target\":\"%s\"", bl_id("task", task));
        for (int i = 0; i < r->nctx; i++) {
            struct emu68k_ctx *ctx = &r->ctx[i];
            if (!ctx->live || ctx->task != task) continue;
            ctx->finished = 1;
            ctx->blocked = 0;
            if (i == r->cur_ctx) {
                if (ctx->can_unwind) longjmp(ctx->unwind, 1);
                st->pc = 0;
                return J5D_LVO_REDIRECT;
            }
            return 0;
        }
        return 0;
    }
    /* Exec list handling operates on GUEST structures, so it is performed in
     * guest memory here rather than handed to the native AROS AddHead, which
     * would manipulate host pointers in a list the program cannot address.
     * (struct Node: ln_Succ at 0, ln_Pred at 4; struct List: lh_Head 0,
     * lh_Tail 4, lh_TailPred 8.) */
    case LVO_ADDHEAD: {
        uint32_t list = st->a[0], node = st->a[1];
        uint32_t head = gread32(sb, list);
        gwrite32(sb, node, head);              /* node->ln_Succ = list->lh_Head */
        gwrite32(sb, node + 4, list);          /* node->ln_Pred = &lh_Head      */
        gwrite32(sb, head + 4, node);          /* head->ln_Pred = node          */
        gwrite32(sb, list, node);              /* list->lh_Head = node          */
        return 0;
    }
    case LVO_ADDTAIL: {
        uint32_t list = st->a[0], node = st->a[1];
        uint32_t tailpred = gread32(sb, list + 8);
        gwrite32(sb, node, list + 4);          /* node->ln_Succ = &lh_Tail      */
        gwrite32(sb, node + 4, tailpred);      /* node->ln_Pred = lh_TailPred   */
        gwrite32(sb, tailpred, node);          /* tailpred->ln_Succ = node      */
        gwrite32(sb, list + 8, node);          /* list->lh_TailPred = node      */
        return 0;
    }
    case LVO_REMOVE: {
        uint32_t node = st->a[1];
        uint32_t succ = gread32(sb, node), pred = gread32(sb, node + 4);
        gwrite32(sb, pred, succ);
        gwrite32(sb, succ + 4, pred);
        return 0;
    }
    /* RemHead/RemTail hand back a NODE, and the node is the guest's own: a
     * native pointer would be an address it cannot dereference, so like the
     * Add* pair these walk guest memory directly. An empty list is detected the
     * way exec does it, by the terminator's NULL link rather than by a count. */
    /* Both operands are GUEST addresses and the payload is plain bytes, so this
     * is a move inside the arena. Bridging it would hand dos.library two guest
     * pointers it cannot dereference. Overlap is allowed: CopyMemQuick promises
     * long-aligned non-overlapping, but a program that gets that wrong should
     * misbehave the way it does on a real Amiga, not corrupt the host. */
    case LVO_COPYMEM:
    case LVO_COPYMEMQUICK: {
        uint32_t src = st->a[0], dst = st->a[1], n = st->d[0];
        if (!n) return 0;
        if (src < sb->sandbox_origin || dst < sb->sandbox_origin ||
            (uint64_t)src + n > (uint64_t)sb->sandbox_origin + sb->size ||
            (uint64_t)dst + n > (uint64_t)sb->sandbox_origin + sb->size) {
            snprintf(e, el, "CopyMem %08x -> %08x (%u bytes) leaves the guest arena",
                     src, dst, n);
            return 1;
        }
        memmove(j4_sandbox_host(sb, dst), j4_sandbox_host(sb, src), n);
        return 0;
    }
    case LVO_SUPERVISOR:
        /* Supervisor(A5) runs the CALLER'S OWN routine with the S bit set. It
         * is not a request for anything the host has to provide: the code is
         * the guest's, in the guest's memory, and the only thing supervisor
         * mode buys it is the right to touch the privileged registers. So it
         * runs, in the guest, exactly where it is - and if it then reaches for
         * something this machine does not have, that is caught there, by name,
         * instead of the whole call being refused for what it might do.
         *
         * The routine returns with rte, so the dispatcher builds the frame. */
        if (!st->a[5]) {                             /* nothing to run          */
            st->d[0] = 0;
            return 0;
        }
        if (emu68k_host_getenv("EMU68K_TRACE_CALLS")) {
            uint32_t target = st->a[5];
            fprintf(stderr, "[68k] Supervisor target=%08x", target);
            if (target >= sb->sandbox_origin &&
                (uint64_t)target + 16u <=
                    (uint64_t)sb->sandbox_origin + sb->size) {
                const uint8_t *p = j4_sandbox_host(sb, target);
                fputs(" bytes=", stderr);
                for (unsigned i = 0; i < 16; i++)
                    fprintf(stderr, "%s%02x", i ? " " : "", p[i]);
            } else {
                fputs(" (outside guest sandbox)", stderr);
            }
            fputc('\n', stderr);
            if (st->pc >= sb->sandbox_origin + 16u &&
                (uint64_t)st->pc + 16u <=
                    (uint64_t)sb->sandbox_origin + sb->size) {
                const uint8_t *p = j4_sandbox_host(sb, st->pc - 16u);
                fprintf(stderr, "[68k] Supervisor caller=%08x bytes[-16..+15]=",
                        st->pc);
                for (unsigned i = 0; i < 32; i++)
                    fprintf(stderr, "%s%02x", i ? " " : "", p[i]);
                fputc('\n', stderr);
            }
        }
        st->pc = st->a[5];
        return J5D_LVO_REDIRECT_RTE;
    case LVO_CACHECLEARU:
    case LVO_CACHECLEARE:
        /* A 68k program clears the caches when it has just written bytes that
         * are about to be executed. Under translation that is the notification
         * that a translated block may no longer match the code it came from,
         * which no amount of host cache maintenance would fix, so the whole
         * per-run translation cache goes. CacheClearE names a range; dropping
         * everything is a superset and a range-precise version would only be
         * faster, never more correct.
         *
         * The return cannot go back into a block that was just freed, so this
         * takes the same exit as the guest-library reclaim: the permanent RTS,
         * reached by redirect, which pops the caller's return address and
         * carries on through freshly translated code. */
        j5d_run_free();
        st->pc = OSCODE_RETURN;
        return J5D_LVO_REDIRECT;
    case LVO_REMHEAD: {
        uint32_t list = st->a[0];
        uint32_t node = gread32(sb, list);           /* lh_Head                */
        uint32_t succ = gread32(sb, node);           /* node->ln_Succ          */
        if (!succ) { st->d[0] = 0; return 0; }       /* the list was empty     */
        gwrite32(sb, list, succ);                    /* lh_Head = succ         */
        gwrite32(sb, succ + 4, list);                /* succ->ln_Pred = &lh_Head */
        st->d[0] = node;
        return 0;
    }
    case LVO_REMTAIL: {
        uint32_t list = st->a[0];
        uint32_t node = gread32(sb, list + 8);       /* lh_TailPred            */
        uint32_t pred = gread32(sb, node + 4);       /* node->ln_Pred          */
        if (!pred) { st->d[0] = 0; return 0; }
        gwrite32(sb, list + 8, pred);                /* lh_TailPred = pred     */
        gwrite32(sb, pred, list + 4);                /* pred->ln_Succ = &lh_Tail */
        st->d[0] = node;
        return 0;
    }
    case LVO_ENQUEUE: {
        /* Priority-sorted insert: walk to the first node of LOWER priority and
         * insert before it. ln_Pri is a SIGNED byte at offset 9. */
        uint32_t list = st->a[0], node = st->a[1];
        int pri = (int8_t)j4_sandbox_host(sb, node)[9];
        uint32_t next = gread32(sb, list);           /* lh_Head                */
        uint32_t succ;
        while ((succ = gread32(sb, next)) != 0) {    /* not yet the terminator */
            if ((int8_t)j4_sandbox_host(sb, next)[9] < pri) break;
            next = succ;
        }
        {   uint32_t pred = gread32(sb, next + 4);
            gwrite32(sb, node, next);                /* node->ln_Succ = next   */
            gwrite32(sb, node + 4, pred);            /* node->ln_Pred = pred   */
            gwrite32(sb, pred, node);
            gwrite32(sb, next + 4, node);
        }
        return 0;
    }
    case LVO_CREATEMSGPORT: {
        /* A guest MsgPort. The program holds it and puts it in structures, so
         * it is built in guest memory - but a port is not just an empty list:
         * a real Exec port OWNS A SIGNAL BIT AND ITS CREATING TASK, which is
         * what PutMsg sets and what a Wait/WaitPort event loop blocks on.
         * Without them every ordinary event loop over this port is a wait on
         * bit zero of nobody. */
        uint32_t port = guest_alloc(r, M68K_MsgPort_SIZEOF);
        uint32_t task = ctx_task(r);
        uint32_t alloc = gread32(sb, task + TASK_SIGALLOC_OFF);
        int bit;
        if (!port) { st->d[0] = 0; return 0; }
        for (bit = 31; bit >= 16; bit--)
            if (!(alloc & (1u << bit))) break;
        if (bit < 16) {
            snprintf(e, el, "capability gap: no free signal for a message port");
            return 1;
        }
        gwrite32(sb, task + TASK_SIGALLOC_OFF, alloc | (1u << bit));
        memset(j4_sandbox_host(sb, port), 0, M68K_MsgPort_SIZEOF);
        j4_sandbox_host(sb, port)[M68K_MsgPort_mp_Node_ln_Type] = 4;  /* NT_MSGPORT */
        *(uint8_t *)j4_sandbox_host(sb, port + MP_SIGBIT) = (uint8_t)bit;
        gwrite32(sb, port + MP_SIGTASK, task);
        gwrite32(sb, port + MP_MSGLIST + M68K_List_lh_Head,
                 port + MP_MSGLIST + M68K_List_lh_Tail);
        gwrite32(sb, port + MP_MSGLIST + M68K_List_lh_Tail, 0);
        gwrite32(sb, port + MP_MSGLIST + M68K_List_lh_TailPred,
                 port + MP_MSGLIST + M68K_List_lh_Head);
        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "port.create",
                 "\"port\":\"%s\",\"owner\":\"%s\",\"signal_bit\":%d",
                 bl_id("port", port), bl_id("task", task), bit);
        st->d[0] = port;
        return 0;
    }
    /* Ports and semaphores. A guest has one thread of control in its own
     * arena, so arbitration is a no-op; what matters is that the STRUCTURES it
     * initialises look right afterwards, because the program walks them.
     * struct SignalSemaphore: ss_Link(0,14) ss_NestCount(14) ss_WaitQueue(16,
     * MinList: head 16, tail 20, tailpred 24) ss_Owner(36) ss_QueueCount(40) */
    case LVO_INITSEMAPHORE: {   /* InitSemaphore(sigSem A0)                     */
        uint32_t ss = st->a[0];
        if (ss) {
            memset(j4_sandbox_host(sb, ss), 0, 44);
            gwrite32(sb, ss + 16, ss + 20);      /* mlh_Head = &mlh_Tail        */
            gwrite32(sb, ss + 20, 0);            /* mlh_Tail = NULL             */
            gwrite32(sb, ss + 24, ss + 16);      /* mlh_TailPred = &mlh_Head    */
            gwrite32(sb, ss + 36, 0);            /* ss_Owner = NULL             */
            j4_sandbox_host(sb, ss)[40] = 0xFF;  /* ss_QueueCount = -1 (WORD)   */
            j4_sandbox_host(sb, ss)[41] = 0xFF;
        }
        return 0;
    }
    /* One cooperative context runs at a time, so nothing contends: obtaining
     * is bookkeeping and attempting always succeeds. Named through the
     * generated constants so a vector number can never drift. */
    case LVO_OBTAINSEMAPHORE:
    case LVO_RELEASESEMAPHORE:
    case LVO_OBTAINSEMAPHORELIST:
    case LVO_RELEASESEMAPHORELIST:
    case LVO_OBTAINSEMAPHORESHARED:
    case LVO_ADDSEMAPHORE:       /* no public semaphore list to join            */
    case LVO_REMSEMAPHORE:
        return 0;
    case LVO_ATTEMPTSEMAPHORE:
    case LVO_ATTEMPTSEMAPHORESHARED:
        st->d[0] = 1;
        return 0;
    case LVO_FINDSEMAPHORE:      /* nothing was added, so nothing is found      */
        st->d[0] = 0;
        return 0;
    case LVO_PROCURE: {
        /* One cooperative context cannot contend with itself. Grant the guest
         * SemaphoreMessage immediately and reply it through its guest port,
         * preserving the asynchronous API without native pointers. */
        uint32_t sem = st->a[0], msg = st->a[1];
        uint32_t reply;
        if (!guest_span_ok(sb, sem, 44u) ||
            !guest_span_ok(sb, msg, M68K_SemaphoreMessage_SIZEOF)) {
            snprintf(e, el, "Procure semaphore or message is outside guest memory");
            return 1;
        }
        gwrite16(sb, msg + SSM_LENGTH_OFF, M68K_SemaphoreMessage_SIZEOF);
        gwrite32(sb, msg + SSM_SEMAPHORE_OFF, sem);
        reply = gread32(sb, msg + MN_REPLYPORT);
        if (reply) {
            st->a[0] = reply;
            st->a[1] = msg;
            if (exec_call(r, sb, LVO_PUTMSG, st, e, el) != 0) return 1;
        }
        st->d[0] = 0;
        return 0;
    }
    case LVO_VACATE: {
        uint32_t msg = st->a[1];
        if (!guest_span_ok(sb, st->a[0], 44u) ||
            !guest_span_ok(sb, msg, M68K_SemaphoreMessage_SIZEOF)) {
            snprintf(e, el, "Vacate semaphore or message is outside guest memory");
            return 1;
        }
        gwrite32(sb, msg + SSM_SEMAPHORE_OFF, 0);
        return 0;
    }
    case LVO_NEWALLOCENTRY:      /* the same allocation, with a failure mask    */
        return exec_call(r, sb, LVO_ALLOCENTRY, st, e, el);
    case LVO_ADDPORT: {
        /* Public ports are run-global, just as they are system-global on a
         * classic single-address-space machine. All cooperating guest
         * processes in this run share the arena, so publishing the guest
         * address preserves both identity and message-list ownership. */
        uint32_t port = st->a[1];
        uint32_t list = port + MP_MSGLIST;
        uint32_t name;
        int slot = -1;
        if (!guest_span_ok(sb, port, 34u)) {
            snprintf(e, el, "AddPort received a port outside guest memory");
            return 1;
        }
        gwrite8(sb, port + M68K_MsgPort_mp_Node_ln_Type, 4); /* NT_MSGPORT */
        gwrite32(sb, list + M68K_List_lh_Head,
                 list + M68K_List_lh_Tail);
        gwrite32(sb, list + M68K_List_lh_Tail, 0);
        gwrite32(sb, list + M68K_List_lh_TailPred,
                 list + M68K_List_lh_Head);
        name = gread32(sb, port + M68K_Node_ln_Name);
        for (int i = 0; i < EMU68K_PUBLIC_PORT_MAX; i++) {
            if (r->public_port[i].live && r->public_port[i].port == port)
                return 0;
            if (!r->public_port[i].live && slot < 0) slot = i;
        }
        if (slot < 0) {
            snprintf(e, el, "capability gap: guest public-port table is full");
            return 1;
        }
        r->public_port[slot].port = port;
        r->public_port[slot].guest_name = name;
        r->public_port[slot].live = 1;
        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "port.publish",
                 "\"port\":\"%s\",\"raw\":\"0x%08x\",\"name\":\"%s\","
                 "\"owner\":\"%s\",\"signal_bit\":%u,\"head\":\"0x%08x\"",
                 bl_id("port", port), port, guest_cstr(sb, name) ? guest_cstr(sb, name) : "",
                 bl_id("task", gread32(sb, port + MP_SIGTASK)),
                 (unsigned)gread8(sb, port + MP_SIGBIT),
                 gread32(sb, port + MP_MSGLIST + M68K_List_lh_Head));
        return 0;
    }
    case LVO_REMPORT: {
        uint32_t port = st->a[1];
        for (int i = 0; i < EMU68K_PUBLIC_PORT_MAX; i++)
            if (r->public_port[i].live && r->public_port[i].port == port) {
                r->public_port[i].live = 0;
                bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                         "port.unpublish", "\"port\":\"%s\"",
                         bl_id("port", port));
                break;
            }
        return 0;
    }
    case LVO_FINDPORT: {
        const char *wanted = guest_cstr(sb, st->a[1]);
        st->d[0] = 0;
        if (!wanted) return 0;
        for (int i = 0; i < EMU68K_PUBLIC_PORT_MAX; i++) {
            const char *published;
            if (!r->public_port[i].live) continue;
            published = guest_cstr(sb, r->public_port[i].guest_name);
            if (published && !strcmp(published, wanted)) {
                st->d[0] = r->public_port[i].port;
                bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "port.find",
                         "\"name\":\"%s\",\"port\":\"%s\"",
                         wanted, bl_id("port", st->d[0]));
                break;
            }
        }
        return 0;
    }

    /* ---- vectors a guest LIBRARY needs while it initialises -----------------
     * A library above the waterline runs its own init code, and that code does
     * things a program rarely does: it patches vectors, checksums itself, asks
     * for resources. Served here because all of it is guest memory. */
    case LVO_SETFUNCTION: {
        /* SetFunction(library A1, funcOffset A0 (word), newFunction D0).
         *
         * A GUEST library's vector table is guest memory: patching it is a
         * write, and the old target is readable. A BRIDGED library has no
         * guest vectors - they are native code - but the patch still has a
         * meaning we can honour exactly: record the guest routine, and route
         * every later call to that vector into it instead of serving it here.
         * That is what the patcher asked for. locale.library patches exec's
         * RawDoFmt this way, which is how its formatting reaches every caller. */
        uint32_t base = st->a[1];
        int32_t off = (int32_t)(int16_t)(uint16_t)st->a[0];
        uint32_t newf = st->d[0];
        if (off >= 0 || (off % 6) != 0) {
            snprintf(e, el, "SetFunction offset %d is not a library vector", off);
            return 1;
        }
        if (find_guestlib_base(r, base) >= 0) {
            uint32_t slot = base + (uint32_t)off;   /* JMP <abs32> */
            uint32_t old = gread32(sb, slot + 2);
            gwrite16(sb, slot, 0x4ef9u);
            gwrite32(sb, slot + 2, newf);
            st->d[0] = old;
            return 0;
        }
        {
            int lvo_patched = (int)((-off) / 6);
            int slot = -1;
            for (int i = 0; i < EMU68K_PATCH_MAX; i++) {
                if (r->patch[i].base == base && r->patch[i].lvo == lvo_patched) {
                    slot = i; break;
                }
                if (slot < 0 && !r->patch[i].base) slot = i;
            }
            if (slot < 0) {
                snprintf(e, el, "more patched vectors than this bridge keeps");
                return 1;
            }
            st->d[0] = r->patch[slot].base ? r->patch[slot].guest_fn : 0;
            r->patch[slot].base = base;
            r->patch[slot].lvo = lvo_patched;
            r->patch[slot].guest_fn = newf;
            bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "library.patch",
                     "\"base\":%u,\"lvo\":%d", base, lvo_patched);
            return 0;
        }
    }
    case LVO_SUMLIBRARY:     /* checksums guard against exactly the patching we
                              * just did; there is no checksum to keep */
        st->d[0] = 0;
        return 0;
    case LVO_OPENRESOURCE: {
        const char *name = guest_cstr(sb, st->a[1]);
        if (emu68k_host_getenv("EMU68K_TRACE_CALLS"))
            fprintf(stderr, "[68k] OpenResource(\"%s\")\n",
                    name ? name : "<invalid guest string>");
        /* AROS compiler startup and libc use task.resource for guest-owned
         * hook and task-local-storage state.  Give it a callable guest facade;
         * emu68k_taskresource_call owns the semantics.  Resources we do not
         * model remain absent, which is the documented OpenResource result. */
        if (name && !strcmp(name, "task.resource")) {
            uint32_t base = (uint32_t)emu68k_run_device_base(r, name);
            if (!base) {
                snprintf(e, el, "guest facade table is full for task.resource");
                return 1;
            }
            if (!gread32(sb, base + M68K_Library_lib_Node_ln_Name)) {
                uint32_t guest_name = guest_strdup(r, name, strlen(name));
                if (!guest_name) {
                    snprintf(e, el, "guest memory exhausted for task.resource");
                    return 1;
                }
                gwrite32(sb, base + M68K_Library_lib_Node_ln_Name, guest_name);
                gwrite16(sb, base + M68K_Library_lib_Version, 1);
                gwrite16(sb, base + M68K_Library_lib_Revision, 3);
            }
            st->d[0] = base;
            return 0;
        }
        st->d[0] = 0;
        return 0;
    }
    case LVO_TYPEOFMEM: {
        /* Everything the guest can address is one arena: public and clearable,
         * never chip. Outside it, 0 - which is what "not memory I gave you"
         * means to a caller. */
        uint32_t addr = st->d[0];
        st->d[0] = (addr >= sb->sandbox_origin &&
                    addr < sb->sandbox_origin + sb->size) ? (1u << 0 | 1u << 2)
                                                          : 0;
        return 0;
    }
    case LVO_FINDNAME: {
        /* FindName(list A0, name A1) over guest memory: the nodes and the
         * strings are the guest's, so a native call could not read either. */
        uint32_t list = st->a[0], want = st->a[1];
        const char *s = guest_cstr(sb, want);
        st->d[0] = 0;
        if (!list || !s) return 0;
        for (uint32_t n = gread32(sb, list); n && gread32(sb, n); n = gread32(sb, n)) {
            uint32_t nm = gread32(sb, n + 10);          /* ln_Name */
            const char *have = nm ? guest_cstr(sb, nm) : NULL;
            if (have && !strcmp(have, s)) { st->d[0] = n; return 0; }
        }
        return 0;
    }
    case LVO_INSERT: {
        /* Insert(list A0, node A1, pred A2): pred NULL or the head sentinel
         * means "at the head", which is how exec documents it. */
        uint32_t list = st->a[0], node = st->a[1], pred = st->a[2];
        uint32_t succ;
        if (!pred || pred == list) {
            succ = gread32(sb, list);
            gwrite32(sb, node, succ);
            gwrite32(sb, node + 4, list);
            gwrite32(sb, succ + 4, node);
            gwrite32(sb, list, node);
            return 0;
        }
        succ = gread32(sb, pred);
        gwrite32(sb, node, succ);
        gwrite32(sb, node + 4, pred);
        gwrite32(sb, succ + 4, node);
        gwrite32(sb, pred, node);
        return 0;
    }
    case LVO_NEWMINLIST: {
        uint32_t list = st->a[0];
        if (!list) return 0;
        gwrite32(sb, list, list + 4);        /* mlh_Head     = &mlh_Tail */
        gwrite32(sb, list + 4, 0);           /* mlh_Tail     = NULL      */
        gwrite32(sb, list + 8, list);        /* mlh_TailPred = &mlh_Head */
        return 0;
    }
    case LVO_ALLOCVECPOOLED:
        st->d[0] = st->d[0] ? guest_alloc(r, st->d[0]) : 0;
        return 0;
    case LVO_FREEVECPOOLED:  /* the arena is a bump heap: freeing is a no-op,
                              * exactly as for FreeVec */
        return 0;
    case LVO_SETTASKPRI:     /* one cooperative context per task: a priority
                              * cannot change who runs, so report the old one */
        st->d[0] = 0;
        return 0;
    case LVO_DEBUG:
    case LVO_ADDRESETCALLBACK:
    case LVO_REMRESETCALLBACK:
        st->d[0] = 0;
        return 0;
    case LVO_CACHECONTROL:   /* no caches to control; report none enabled */
        st->d[0] = 0;
        return 0;

    /* ---- registration: a guest library announcing itself --------------------
     * The run already knows every library it loaded, so these succeed without
     * a system list to join. Refusing them would stop a library at its own
     * init for bookkeeping we do not need. */
    case LVO_ADDLIBRARY:
    case LVO_ADDDEVICE:
    case LVO_ADDRESOURCE:
        st->d[0] = st->a[1];
        return 0;
    case LVO_REMLIBRARY:
    case LVO_REMDEVICE:
    case LVO_REMRESOURCE:
    case LVO_INITCODE:       /* no resident modules to run */
        st->d[0] = 0;
        return 0;

    /* ---- memory shapes the arena can answer -------------------------------- */
    case LVO_DEALLOCATE:     /* the arena is a bump heap, like FreeMem */
        st->d[0] = 0;
        return 0;
    case LVO_ALLOCENTRY: {
        /* AllocEntry(MemList A0) -> MemList: satisfy every entry from the
         * arena and answer a guest MemList with the addresses filled in. */
        uint32_t ml = st->a[0];
        uint32_t n = gread16(sb, ml + 14);           /* ml_NumEntries */
        uint32_t out = guest_alloc(r, 16 + n * 8);
        if (!out) { st->d[0] = 0x80000000u; return 0; }   /* MEMF_ANY failure */
        gwrite16(sb, out + 14, (uint16_t)n);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t len = gread32(sb, ml + 16 + i * 8 + 4);
            uint32_t p = len ? guest_alloc(r, len) : 0;
            if (len && !p) { st->d[0] = 0x80000000u; return 0; }
            gwrite32(sb, out + 16 + i * 8, p);
            gwrite32(sb, out + 16 + i * 8 + 4, len);
        }
        st->d[0] = out;
        return 0;
    }
    case LVO_FREEENTRY:      /* bump heap: nothing to give back */
        return 0;

    /* ---- raw I/O: the debug path a library uses before anything else ------- */
    case LVO_RAWIOINIT:
        /* The hosted sink is always ready; there is no hardware UART to init. */
        return 0;
    case LVO_RAWMAYGETCHAR:
        /* Raw input is deliberately nonblocking. No hosted raw-input source is
         * attached, which is the documented -1/no-character result. */
        st->d[0] = UINT32_MAX;
        return 0;
    case LVO_RAWPUTCHAR: {
        char ch = (char)(st->d[0] & 0xff);
        if (ch && r->lib.outlen < (int)sizeof(r->lib.out) - 1)
            r->lib.out[r->lib.outlen++] = ch;
        return 0;
    }

    case LVO_CREATEIOREQUEST: {
        /* CreateIORequest(port A0, size D0): guest memory with the reply port
         * filled in, which is all a guest needs before OpenDevice. */
        uint32_t size = st->d[0], req;
        if (!size) { st->d[0] = 0; return 0; }
        req = guest_alloc(r, size);
        if (req) {
            memset(j4_sandbox_host(sb, req), 0, size);
            gwrite32(sb, req + 14, st->a[0]);        /* mn_ReplyPort */
            gwrite16(sb, req + 18, (uint16_t)size);  /* mn_Length    */
        }
        st->d[0] = req;
        return 0;
    }
    case LVO_DELETEIOREQUEST:
        return 0;

    /* ---- what a library's own init code does ------------------------------
     * MakeLibrary/MakeFunctions/InitStruct are how a 68k library builds
     * ANOTHER library (a datatype, a class, a handler) at run time. All three
     * work entirely in guest memory, and the shapes are the ones the T3e
     * loader already lays down for a library it loads from disk. */
    case LVO_MAKEFUNCTIONS: {
        /* MakeFunctions(target A0, functionArray A1, funcDispBase A2):
         * A2 NULL means the array holds absolute longwords, otherwise
         * word-sized displacements from A2. Returns the vector-table size. */
        uint32_t target = st->a[0], array = st->a[1], dispbase = st->a[2];
        uint32_t n = 0;
        for (;;) {
            uint32_t fn;
            if (dispbase) {
                int16_t d = (int16_t)gread16(sb, array + n * 2);
                if ((uint16_t)d == 0xffffu) break;
                fn = d ? dispbase + (uint32_t)(int32_t)d : 0;
            } else {
                fn = gread32(sb, array + n * 4);
                if (fn == 0xffffffffu) break;
            }
            gwrite16(sb, target - (n + 1) * 6, 0x4ef9u);
            gwrite32(sb, target - (n + 1) * 6 + 2, fn);
            n++;
            if (n > 4096) {
                snprintf(e, el, "MakeFunctions: vector array is not terminated");
                return 1;
            }
        }
        st->d[0] = n * 6;
        return 0;
    }
    case LVO_MAKELIBRARY: {
        /* MakeLibrary(vectors A0, structure A1, init A2, dataSize D0,
         * segList D1) -> base. The base sits above its own vector table, so
         * the allocation covers both and the returned address is the middle. */
        uint32_t vectors = st->a[0], structure = st->a[1];
        uint32_t dsize = st->d[0];
        uint32_t nvec = 0;
        for (;;) {
            uint32_t fn = gread32(sb, vectors + nvec * 4);
            if (fn == 0xffffffffu) break;
            nvec++;
            if (nvec > 4096) {
                snprintf(e, el, "MakeLibrary: vector array is not terminated");
                return 1;
            }
        }
        {
            uint32_t negsize = nvec * 6;
            uint32_t mem = guest_alloc(r, negsize + dsize);
            uint32_t base;
            if (!mem) { st->d[0] = 0; return 0; }
            memset(j4_sandbox_host(sb, mem), 0, negsize + dsize);
            base = mem + negsize;
            for (uint32_t i = 0; i < nvec; i++) {
                uint32_t fn = gread32(sb, vectors + i * 4);
                gwrite16(sb, base - (i + 1) * 6, 0x4ef9u);
                gwrite32(sb, base - (i + 1) * 6 + 2, fn);
            }
            gwrite16(sb, base + 16, (uint16_t)negsize);   /* lib_NegSize */
            gwrite16(sb, base + 18, (uint16_t)dsize);     /* lib_PosSize */
            if (structure) {
                st->a[0] = base;                          /* InitStruct(A1,A2) */
                st->a[1] = structure;
                st->a[2] = base;
                st->d[0] = dsize;
                /* fall through to the same interpreter InitStruct uses */
                lvo = LVO_INITSTRUCT;
                {
                    int rc = exec_call(r, sb, LVO_INITSTRUCT, st, e, el);
                    if (rc) return rc;
                }
            }
            st->d[0] = base;
        }
        return 0;
    }
    case LVO_INITSTRUCT: {
        /* InitStruct(initTable A1, memory A2, size D0): the classic byte-code
         * that fills a structure. Everything it touches is guest memory. */
        uint32_t table = st->a[1], mem = st->a[2], size = st->d[0];
        uint32_t p = table, dest = mem;
        if (size) memset(j4_sandbox_host(sb, mem), 0, size);
        for (int guard = 0; guard < 65536; guard++) {
            uint8_t cmd = gread8(sb, p++);
            uint32_t count, off;
            if (!cmd) break;
            count = (cmd & 0x3f) + 1;
            if (cmd & 0x80) {                    /* offset follows */
                if (cmd & 0x40) { off = gread16(sb, p); p += 2; }
                else            { off = gread8(sb, p); p += 1; }
                dest = mem + off;
            }
            switch ((cmd >> 6) & 3) {
            case 0:                              /* bytes */
                for (uint32_t i = 0; i < count; i++)
                    gwrite16(sb, dest, gread8(sb, p++)), dest += 1;
                p = (p + 1) & ~1u;
                break;
            case 1:                              /* words */
                for (uint32_t i = 0; i < count; i++) {
                    gwrite16(sb, dest, gread16(sb, p)); p += 2; dest += 2;
                }
                break;
            default:                             /* longs */
                for (uint32_t i = 0; i < count; i++) {
                    gwrite32(sb, dest, gread32(sb, p)); p += 4; dest += 4;
                }
                break;
            }
        }
        st->d[0] = 0;
        return 0;
    }

    case LVO_ADDMEMLIST: {
        uint32_t size = st->d[0], attr = st->d[1], base = st->a[0];
        uint32_t chunk = base + M68K_MemHeader_SIZEOF;
        if (size <= M68K_MemHeader_SIZEOF || base < r->exec_heap ||
            !guest_span_ok(sb, base, size)) {
            snprintf(e, el, "AddMemList range %08x+%u is not free guest memory",
                     base, size);
            return 1;
        }
        memset(j4_sandbox_host(sb, base), 0, M68K_MemHeader_SIZEOF);
        gwrite8(sb, base + M68K_Node_ln_Type, 10);       /* NT_MEMORY */
        gwrite8(sb, base + M68K_Node_ln_Pri, (uint8_t)st->d[2]);
        gwrite32(sb, base + M68K_Node_ln_Name, st->a[1]);
        gwrite16(sb, base + MH_ATTR_OFF, attr);
        gwrite32(sb, base + MH_FIRST_OFF, chunk);
        gwrite32(sb, base + MH_LOWER_OFF, chunk);
        gwrite32(sb, base + MH_UPPER_OFF, base + size);
        gwrite32(sb, base + MH_FREE_OFF, size - M68K_MemHeader_SIZEOF);
        gwrite32(sb, chunk + MC_NEXT_OFF, 0);
        gwrite32(sb, chunk + MC_BYTES_OFF, size - M68K_MemHeader_SIZEOF);
        /* The arena allocator already owns the encompassing guest address
         * space. Skip the MemHeader itself so later AllocMem calls can consume
         * the newly announced bytes without overwriting its metadata. */
        r->exec_heap = chunk;
        if (r->sb.next_alloc < r->exec_heap) r->sb.next_alloc = r->exec_heap;
        return 0;
    }

    /* ---- CPU/system state a guest may ask about --------------------------- */
    case LVO_SUPERSTATE:     /* there is no supervisor mode to enter; the old
                              * SR of 0 is what UserState is given back */
        st->d[0] = 0;
        return 0;
    case LVO_USERSTATE:
        return 0;
    case LVO_GETCC:          /* the condition codes the guest just computed */
        st->d[0] = j5d_pack_sr(st) & 0x1f;
        return 0;
    case LVO_CACHEPREDMA:    /* no caches: the address is already coherent */
    case LVO_CACHEPOSTDMA:
        return 0;
    case LVO_SUMKICKDATA:
        st->d[0] = 0;
        return 0;
    case LVO_ADDMEMHANDLER:  /* a low-memory handler: the arena never asks one
                              * to free anything, so it is registered and never
                              * called. diskfont installs one while it opens. */
        st->d[0] = st->a[1];
        return 0;
    case LVO_REMMEMHANDLER:
        return 0;
    case LVO_FINDTASKBYPID:  /* one process per run; no PID namespace */
        st->d[0] = 0;
        return 0;
    case LVO_CHILDFREE:
        /* Guest Tasks do not carry an ETask child registry, matching the
         * native no-op when there is no new-style child record. */
        return 0;
    case LVO_CHILDORPHAN:
    case LVO_CHILDSTATUS:
    case LVO_CHILDWAIT:
        st->d[0] = 1;        /* CHILD_NOTNEW from exec/tasks.h */
        return 0;
    case LVO_OBTAINQUICKVECTOR:
    case LVO_READGAYLE:
        /* AROS's portable implementations return zero when the hardware
         * facility is absent. The hosted guest has no Gayle/quick vector. */
        st->d[0] = 0;
        return 0;
    case LVO_ALERT:          /* an alert is the program declaring it cannot go
                              * on: say so with its code rather than continue */
        snprintf(e, el, "the program raised Alert %08x", st->d[0]);
        return 1;
    case LVO_VNEWRAWDOFMT:   /* same engine, same reason it runs in the guest */
        st->pc = OSCODE_RAWDOFMT;
        return J5D_LVO_REDIRECT;
    case LVO_DELETEMSGPORT: {
        /* The memory is a bump heap, but the SIGNAL is a real resource: a
         * program that creates and deletes ports in a loop runs out of bits. */
        uint32_t port = st->a[0];
        if (port) {
            emu68k_event_unbind_port(r, port, "DeleteMsgPort");
            uint32_t task = gread32(sb, port + MP_SIGTASK);
            uint32_t bit = gread8(sb, port + MP_SIGBIT);
            if (task && bit < 32)
                gwrite32(sb, task + TASK_SIGALLOC_OFF,
                         gread32(sb, task + TASK_SIGALLOC_OFF) & ~(1u << bit));
            bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "port.delete",
                     "\"port\":\"%s\",\"signal_bit\":%d",
                     bl_id("port", port), (int)bit);
        }
        return 0;
    }
    case LVO_SETSIGNAL: {
        /* SetSignal(newSignals D0, signalMask D1) -> old received signals.
         *
         * Ports, Signal(), and native IDCMP delivery all now set tc_SigRecvd
         * in the shared guest Task.  Keeping the old startup-era zero stub
         * here made users of the normal Exec signal protocol (notably ARexx
         * worker/reply loops) observe a different state from Wait().  The
         * hosted executor is cooperative, so this read-modify-write is the
         * required atomic operation for guest code: replace only the bits in
         * D1, return the complete old value in D0. */
        uint32_t task = ctx_task(r);
        uint32_t old = gread32(sb, task + TASK_SIGRECVD_OFF);
        uint32_t mask = st->d[1];
        gwrite32(sb, task + TASK_SIGRECVD_OFF,
                 (old & ~mask) | (st->d[0] & mask));
        st->d[0] = old;
        return 0;
    }
    /* A signal BIT is bookkeeping: a program reserves one for a port it is
     * about to create, long before anything is ever sent to it. Refusing the
     * reservation stopped programs during startup, at their ARexx port, over a
     * bit nobody had signalled yet. What a guest still cannot do is WAIT on
     * one, and that stays a named gap rather than a hang. */
    /* ---- MESSAGE PORTS ------------------------------------------------------
     *
     * Every structure involved - the MsgPort, the Message, the list linking
     * them - is the GUEST's own memory, so these are guest-memory list
     * operations exactly like the Add/Rem pair above, plus a signal. Handing
     * them to the native exec would give it guest addresses it cannot
     * dereference, and would put the program's messages on a list it cannot
     * see.
     *
     * A port's list is initialised by the program with NewList, so these read
     * the same lh_Head/lh_Tail/lh_TailPred layout exec does, and an empty list
     * is detected the way exec detects it: by the terminator's NULL link. */
    case LVO_PUTMSG: {
        uint32_t port = st->a[0], msg = st->a[1];
        uint32_t list = port + MP_MSGLIST;
        uint32_t port_task = gread32(sb, port + MP_SIGTASK);
        uint32_t port_bit = gread8(sb, port + MP_SIGBIT);
        uint32_t sig_before = (port_task && port_bit < 32)
            ? gread32(sb, port_task + TASK_SIGRECVD_OFF) : 0;
        uint32_t head_before = gread32(sb, list + M68K_List_lh_Head);
        uint32_t tailpred = gread32(sb, list + M68K_List_lh_TailPred);
        uint32_t reply = gread32(sb, msg + MN_REPLYPORT);
        uint16_t length = gread16(sb, msg + M68K_Message_mn_Length);
        uint32_t word20 = 0, word24 = 0, word28 = 0, word32 = 0, word36 = 0;
        uint32_t word40 = 0, word44 = 0;
        /* The message header is universal Exec ABI.  The words following it
         * deliberately remain untyped: recording them lets Bridge Lab
         * correlate protocols such as ARexx without pretending every message
         * is one.  Read only a fully guest-owned 40-byte prefix. */
        if (guest_span_ok(sb, msg, 48)) {
            word20 = gread32(sb, msg + 20);
            word24 = gread32(sb, msg + 24);
            word28 = gread32(sb, msg + 28);
            word32 = gread32(sb, msg + 32);
            word36 = gread32(sb, msg + 36);
            word40 = gread32(sb, msg + 40);
            word44 = gread32(sb, msg + 44);
        }
        /* Classic applications still interpret RexxMsg word 24 as
         * rm_LibBase and call rexxsyslib through it. AROS calls the word
         * rm_Private2; Regina stores its TSD pointer there. For a complete
         * RexxMsg with a real Rexx action, make the private value a semantic
         * alias for the loaded guest rexxsyslib base. The message itself stays
         * untouched so Regina gets its TSD back with the reply. */
        if (length >= 128u && word24 &&
            (((word28 >> 24) >= 1u && (word28 >> 24) <= 13u) ||
             (word28 >> 24) >= 0xf0u)) {
            int gi = find_guestlib_name(r, "rexxsyslib.library");
            if (gi >= 0 && r->guestlib[gi].state == GL_READY &&
                find_guestlib_base(r, word24) < 0 &&
                emu68k_register_libalias(r, word24,
                                         r->guestlib[gi].base, e, el) != 0)
                return 1;
        }
        list = port + MP_MSGLIST;
        tailpred = gread32(sb, list + M68K_List_lh_TailPred);
        gwrite32(sb, msg, list + M68K_List_lh_Tail);
        gwrite32(sb, msg + 4, tailpred);
        gwrite32(sb, tailpred, msg);
        gwrite32(sb, list + M68K_List_lh_TailPred, msg);
        /* and tell the port's task, which is what makes a WaitPort return */
        {
            uint32_t task = gread32(sb, port + MP_SIGTASK);
            uint32_t bit  = gread8(sb, port + MP_SIGBIT);
            if (task && bit < 32)
                gwrite32(sb, task + TASK_SIGRECVD_OFF,
                         gread32(sb, task + TASK_SIGRECVD_OFF) | (1u << bit));
            uint32_t sig_after = (task && bit < 32)
                ? gread32(sb, task + TASK_SIGRECVD_OFF) : 0;
            bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "port.put",
                     "\"port\":\"%s\",\"message\":\"%s\","
                     "\"reply_port\":\"%s\",\"owner\":\"%s\","
                     "\"signal_bit\":%u,\"length\":%u,"
                     "\"word20\":\"0x%08x\",\"word24\":\"0x%08x\","
                     "\"word28\":\"0x%08x\",\"word32\":\"0x%08x\","
                     "\"word36\":\"0x%08x\",\"word40\":\"0x%08x\","
                     "\"word44\":\"0x%08x\",\"head_before\":\"0x%08x\","
                     "\"tailpred_before\":\"0x%08x\",\"sig_before\":\"0x%08x\","
                     "\"sig_after\":\"0x%08x\"",
                     bl_id("port", port), bl_id("message", msg),
                     reply ? bl_id("port", reply) : "",
                     bl_id("task", task), (unsigned)bit, (unsigned)length,
                     word20, word24, word28, word32, word36, word40, word44,
                     head_before, tailpred, sig_before, sig_after);
            if (emu68k_trace_tasks())
                fprintf(stderr, "[68k/task] ctx=%d task=%08x pc=%08x PutMsg "
                        "port=%08x msg=%08x head=%08x tailpred=%08x owner=%08x "
                        "bit=%u received=%08x\n",
                        r->cur_ctx, ctx_task(r), st->pc, port, msg,
                        gread32(sb, list + M68K_List_lh_Head),
                        gread32(sb, list + M68K_List_lh_TailPred), task,
                        (unsigned)bit,
                        task ? gread32(sb, task + TASK_SIGRECVD_OFF) : 0);
        }
        return 0;
    }
    case LVO_OPENDEVICE: {
        /* OpenDevice(name A0, unit D0, ioRequest A1, flags D1) -> 0 on success.
         *
         * Opened for real, on the AROS side: a device this system does not have
         * must fail the way a missing device fails, not succeed and then behave
         * oddly. What the guest gets back is a base in io_Device - the same
         * facade a bridged library gets - so the device's VECTORS arrive here
         * like any other call.
         *
         * That is deliberately all this contract promises. Sending a COMMAND to
         * the device is a separate contract (a request queue, DoIO/SendIO/
         * AbortIO, and a marshalled IORequest per command), and each unserved
         * command is refused by name at the point it is sent - so a program
         * that opens a device defensively and never uses it runs, and one that
         * really does I/O stops at the exact command it needed. */
        {
            const char *name = guest_cstr(sb, st->a[0]);
            uint32_t request = st->a[1];
            if (!name) {
                snprintf(e, el, "OpenDevice: name pointer A0=%08x is outside "
                         "guest memory", st->a[0]);
                return 1;
            }
            if (!guest_span_ok(sb, request, M68K_IORequest_SIZEOF)) {
                snprintf(e, el, "OpenDevice(\"%s\"): IORequest A1=%08x is "
                         "outside guest memory", name, request);
                return 1;
            }
            if (!strcmp(name, "timer.device")) {
                uint32_t base = (uint32_t)emu68k_run_device_base(r, name);
                if (!base) {
                    gwrite8(sb, request + M68K_IORequest_io_Error, 1);
                    st->d[0] = UINT32_MAX;
                    return 0;
                }
                gwrite32(sb, request + M68K_IORequest_io_Device, base);
                gwrite32(sb, request + M68K_IORequest_io_Unit, 0);
                gwrite8(sb, request + M68K_IORequest_io_Error, 0);
                st->d[0] = 0;
                return 0;
            }
            if (g_oscall && g_oscall("exec.library", lvo, st, r->reserve,
                                     g_oscall_user, e, el) == 0)
                return 0;
            snprintf(e, el, "capability gap: exec.library OpenDevice(\"%s\") is "
                     "not available yet", name);
        }
        return 1;
    }
    case LVO_CLOSEDEVICE:
        if (guest_span_ok(sb, st->a[1], M68K_IORequest_SIZEOF)) {
            uint32_t base = gread32(sb, st->a[1] + M68K_IORequest_io_Device);
            for (int i = 0; i < r->nlib; i++)
                if (r->openlib[i].base == base &&
                    !strcmp(r->openlib[i].name, "timer.device")) {
                    gwrite32(sb, st->a[1] + M68K_IORequest_io_Device, 0);
                    gwrite32(sb, st->a[1] + M68K_IORequest_io_Unit, 0);
                    gwrite8(sb, st->a[1] + M68K_IORequest_io_Error, 0);
                    st->d[0] = 0;
                    return 0;
                }
        }
        if (g_oscall && g_oscall("exec.library", lvo, st, r->reserve,
                                 g_oscall_user, e, el) == 0)
            return 0;
        return 0;                       /* closing what we never opened is fine */
    case LVO_DOIO: case LVO_SENDIO: case LVO_ABORTIO:
    case LVO_CHECKIO: case LVO_WAITIO:
        if (g_oscall && g_oscall("exec.library", lvo, st, r->reserve,
                                 g_oscall_user, e, el) == 0)
            return 0;
        ledger_record(lvo, r->name[0] ? r->name : NULL);
        if (guest_span_ok(sb, st->a[1], M68K_IORequest_SIZEOF)) {
            uint32_t base = gread32(sb,
                st->a[1] + M68K_IORequest_io_Device);
            uint32_t command = gread16(sb,
                st->a[1] + M68K_IORequest_io_Command);
            const char *device = "unknown device";
            for (int i = 0; i < r->nlib; i++)
                if (r->openlib[i].base == base) {
                    device = r->openlib[i].name;
                    break;
                }
            snprintf(e, el, "capability gap: %s command %u (0x%04x) is not "
                     "marshalled for exec LVO %d", device, command, command,
                     lvo);
        } else {
            snprintf(e, el, "capability gap: device IORequest A1=%08x is "
                     "outside guest memory (exec LVO %d)", st->a[1], lvo);
        }
        return 1;
    case LVO_WAITPORT: {
        /* WaitPort(port A0) -> the first message, LEFT ON the port.
         *
         * Written in terms of the two pieces that already exist: look, and if
         * there is nothing, wait on the bit the port names - which is the bit
         * PutMsg sets. Waiting is where a context hands its turn back, so this
         * is also where a program that is only waiting stops holding the
         * machine. */
        uint32_t port = st->a[0];
        uint32_t list = port + MP_MSGLIST;
        trace_port_call(r, "WaitPort port", st, port);
        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "port.wait",
                 "\"port\":\"%s\"", bl_id("port", port));
        for (;;) {
            uint32_t head;
            event_pump(r, st, port, 0, NULL);
            head = gread32(sb, list + M68K_List_lh_Head);
            if (head && gread32(sb, head)) { st->d[0] = head; return 0; }
            {
                uint32_t bit = gread8(sb, port + MP_SIGBIT);
                int rc;
                if (bit > 31) {
                    snprintf(e, el, "capability gap: WaitPort on a port with no "
                                    "signal bit");
                    return 1;
                }
                st->d[0] = 1u << bit;
                rc = exec_call(r, sb, LVO_WAIT, st, e, el);
                if (rc != 0) return rc;
            }
        }
    }
    case LVO_GETMSG: {
        uint32_t port = st->a[0];
        trace_port_call(r, "GetMsg port", st, port);
        bl_event(BL_DEBUG, r->cur_ctx, ctx_task(r), st->pc, "port.get",
                 "\"port\":\"%s\"", bl_id("port", port));
        event_pump(r, st, port, 0, NULL); /* polling programs need delivery */
        uint32_t list = port + MP_MSGLIST;
        uint32_t head = gread32(sb, list + M68K_List_lh_Head);
        uint32_t succ = head ? gread32(sb, head) : 0;
        bl_event(BL_DEBUG, r->cur_ctx, ctx_task(r), st->pc, "port.get.state",
                 "\"port\":\"%s\",\"raw\":\"0x%08x\",\"owner\":\"%s\","
                 "\"signal_bit\":%u,\"head\":\"0x%08x\",\"succ\":\"0x%08x\"",
                 bl_id("port", port), port, bl_id("task", gread32(sb, port + MP_SIGTASK)),
                 (unsigned)gread8(sb, port + MP_SIGBIT), head, succ);
        if (emu68k_trace_tasks())
            fprintf(stderr, "[68k/task] ctx=%d task=%08x pc=%08x GetMsg queue "
                    "port=%08x head=%08x succ=%08x tailpred=%08x\n",
                    r->cur_ctx, ctx_task(r), st->pc, port, head, succ,
                    gread32(sb, list + M68K_List_lh_TailPred));
        if (!head || !succ) { st->d[0] = 0; return 0; }   /* empty */
        gwrite32(sb, list + M68K_List_lh_Head, succ);
        gwrite32(sb, succ + 4, list);
        st->d[0] = head;
        {
            int idcmp = port_has_event_kind(r, port, EMU68K_EVENT_IDCMP) &&
                        guest_span_ok(sb, head, M68K_IntuiMessage_SIZEOF);
            uint32_t cls = idcmp
                         ? gread32(sb, head + M68K_IntuiMessage_Class) : 0;
            int level = (idcmp && cls == 0x00400000u)
                      ? BL_DEBUG : BL_RUNTIME;
            bl_event(level, r->cur_ctx, ctx_task(r), st->pc, "port.take",
                 "\"port\":\"%s\",\"message\":\"%s\"",
                 bl_id("port", port), bl_id("message", head));
            if (idcmp)
                bl_event(level, r->cur_ctx, ctx_task(r), st->pc,
                     "idcmp.take",
                     "\"port\":\"%s\",\"message\":\"%s\","
                     "\"class\":\"0x%08x\",\"code\":\"0x%04x\","
                     "\"qualifier\":\"0x%04x\",\"window\":\"%s\","
                     "\"iaddress\":\"0x%08x\",\"mouse_x\":%d,"
                     "\"mouse_y\":%d",
                     bl_id("port", port), bl_id("message", head),
                     gread32(sb, head + M68K_IntuiMessage_Class),
                     gread16(sb, head + M68K_IntuiMessage_Code),
                     gread16(sb, head + M68K_IntuiMessage_Qualifier),
                     bl_id("window", gread32(sb,
                         head + M68K_IntuiMessage_IDCMPWindow)),
                     gread32(sb, head + M68K_IntuiMessage_IAddress),
                     (int16_t)gread16(sb, head + M68K_IntuiMessage_MouseX),
                     (int16_t)gread16(sb, head + M68K_IntuiMessage_MouseY));
        }
        return 0;
    }
    case LVO_REPLYMSG: {
        uint32_t msg = st->a[1];
        if (emu68k_host_getenv("EMU68K_TRACE_REXX") &&
            guest_span_ok(sb, msg, 44u)) {
            uint32_t action = gread32(sb, msg + 28u);
            uint32_t result1 = gread32(sb, msg + 32u);
            uint32_t result2 = gread32(sb, msg + 36u);
            uint32_t arg0 = gread32(sb, msg + 40u);
            fprintf(stderr, "[68k/rexx] ReplyMsg ctx=%d task=%08x msg=%08x "
                    "action=%08x result1=%08x result2=%08x arg0=%08x\n",
                    r->cur_ctx, ctx_task(r), msg, action, result1, result2, arg0);
        }
        if (g_oscall && g_oscall("exec.library", lvo, st, r->reserve,
                                 g_oscall_user, e, el) == 0) {
            /* The OS-side bridge recognised a native Intuition message.  It
             * has already returned it to its real native owner; recording
             * this distinction makes a missing ARexx/IPC reply diagnosable
             * without confusing it with a guest-owned message queue. */
            bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                     "port.reply.native", "\"message\":\"%s\"",
                     bl_id("message", msg));
            return 0;
        }
        uint32_t reply = gread32(sb, msg + MN_REPLYPORT);
        if (!reply) {
            bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                     "port.reply.drop", "\"message\":\"%s\"",
                     bl_id("message", msg));
            return 0;                       /* a message with nowhere to go back */
        }
        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "port.reply",
                 "\"message\":\"%s\",\"port\":\"%s\"",
                 bl_id("message", msg), bl_id("port", reply));
        st->a[0] = reply;
        st->a[1] = msg;
        return exec_call(r, sb, LVO_PUTMSG, st, e, el);
    }
    case LVO_WAIT: {
        /* Wait(signalSet D0) -> the signals that arrived.
         *
         * A signal a program sent to ITSELF is already there, which is the
         * common shape for "wake me when I have queued my own work" and needs
         * nothing else to run. Anything else needs a second 68k context to do
         * the signalling, and until one exists a wait that cannot be satisfied
         * would be an unbreakable hang inside a translated program. Naming it
         * is the honest answer: a hang tells the reader nothing, and the run
         * would have to be killed from outside to find out why. */
        uint32_t want = st->d[0];
        uint32_t got;
        if (emu68k_trace_tasks() && want == 0xc0000000u &&
            guest_span_ok(sb, st->a[5], 630u)) {
            uint32_t base = st->a[5];
            fprintf(stderr, "[68k/task] ctx=%d task=%08x Wait-debug a5=%08x "
                    "p574=%08x p582=%08x p590=%08x p598=%08x p602=%08x "
                    "p610=%08x p618=%08x s614=%08x s622=%08x\n",
                    r->cur_ctx, ctx_task(r), base,
                    gread32(sb, base + 574), gread32(sb, base + 582),
                    gread32(sb, base + 590), gread32(sb, base + 598),
                    gread32(sb, base + 602), gread32(sb, base + 610),
                    gread32(sb, base + 618), gread32(sb, base + 614),
                    gread32(sb, base + 622));
        }
        trace_port_call(r, "Wait mask", st, want);
        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc, "signal.wait",
                 "\"mask\":\"0x%08x\"", want);
        /* Before answering, give every port bound to a window a chance to
         * deliver. This is the point the ordinary event loop reaches - Wait
         * comes BEFORE GetMsg - so pumping only at GetMsg would serve a program
         * that polls and never one that blocks. Only ports the program bound to
         * a window are touched; a worker's mailbox is not one of them. */
        event_pump(r, st, 0, want, NULL);
        got = gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF) & want;
        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                 "signal.wait.check", "\"mask\":\"0x%08x\",\"got\":\"0x%08x\"",
                 want, got);
        if (got) {
            gwrite32(sb, ctx_task(r) + TASK_SIGRECVD_OFF,
                     gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF) & ~got);
            st->d[0] = got;
            return 0;
        }
        /* Nothing for us yet.
         *
         * If we are running NESTED - a context someone else gave a turn to -
         * the answer is not to look for work but to hand the turn back. Park
         * on the mask and unwind to whoever ran us; they resume us when the
         * signal arrives, and Wait returns then. Without this a child would
         * spin inside its own WaitPort forever and the parent would never get
         * to send it anything. */
        if (r->ctx[r->cur_ctx].can_unwind) {
            struct emu68k_ctx *me = &r->ctx[r->cur_ctx];
            me->blocked = 1;
            me->wait_mask = want;
            bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                     "scheduler.yield", "\"reason\":\"Wait\",\"mask\":\"0x%08x\"",
                     want);
            /* The JIT knows whether this vector was reached by jsr, jmp, or an
             * rts trampoline.  Let it save the exact continuation before the
             * context yields; guessing a return address here restarted every
             * child at its entry point after its first Wait. */
            return J5D_LVO_BLOCK;
        }
        /* Nothing for us yet. Give the other contexts a turn; one of them is
         * why we are waiting. Each runs until it blocks back or finishes, and
         * we recheck after every one. A context already NESTED BELOW US cannot
         * be run again - that is a genuine deadlock, and recursing into it
         * would turn a diagnosable cycle into a stack overflow. */
        {
            /* A worker that creates an image can execute millions of guest
             * instructions before replying.  The old 64-sweep ceiling was a
             * startup-fixture bound (64 blocks per child turn = only 4096
             * translated blocks total), so it diagnosed a live, advancing
             * PhotoDemo worker as impossible.  Keep the turns short so a
             * signal is observed promptly, but allow real work to finish.
             * Periodically enter native WaitTOF so Intuition/input and other
             * AROS tasks remain schedulable while the parent is inside Wait.
             * The engine poll still enforces kill_req and any corpus deadline;
             * the large final bound retains a loud outcome for a child that
             * polls forever when no deadline was requested. */
            unsigned spun = 0;
            int i;
            for (;;) {
                int progress = 0;
                for (i = 0; i < r->nctx; i++) {
                    struct emu68k_ctx *o = &r->ctx[i];
                    if (i == r->cur_ctx || !o->live || o->finished ||
                        o->on_stack)
                        continue;
                    if (o->blocked &&
                        !(gread32(sb, o->task + TASK_SIGRECVD_OFF) & o->wait_mask))
                        continue;                 /* nothing for it either     */
                    if (run_context_nested(r, sb, i, e, el) != 0)
                        return 1;
                    progress = 1;
                    got = gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF) & want;
                    if (got) {
                        gwrite32(sb, ctx_task(r) + TASK_SIGRECVD_OFF,
                                 gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF)
                                 & ~got);
                        st->d[0] = got;
                        return 0;
                    }
                }
                if (!progress) break;
                spun++;
                if ((spun & 255u) == 0u && emu68k_oscall) {
                    struct j5d_m68k_state waitst = *st;
                    char scratch[128] = {0};
                    (void)emu68k_oscall("graphics.library",
                                       GRAPHICS_LVO_WAITTOF, &waitst,
                                       r->reserve, emu68k_oscall_user,
                                       scratch, sizeof scratch);
                }
                if (r->kill_req || spun >= 65536u) break;
            }
        }
        /* A top-level GUI task can now be waiting only for a native window
         * event.  That is not a capability gap: idle one tick at a time
         * THROUGH THE OS, pump the bound IDCMP ports, and return only when the
         * requested guest signal really exists.  The idle has to be an AROS
         * wait: the tasks that produce that event share this task's CPU. */
        {
            int interactive = 0;
            unsigned matched = 0;
            int pumped = event_pump(r, st, 0, want, &matched);
            /* The authoritative source table lives on the AROS side, where
             * native Intuition/device objects are owned.  `matched` is the
             * typed, cross-boundary fact that at least one of those sources
             * can wake this guest; do not infer it from host-side bookkeeping. */
            interactive = matched != 0;
            if (!interactive) {
                unsigned global_matched = 0;
                int global_pumped = event_pump(r, st, 0, UINT32_MAX,
                                                &global_matched);
                /* The task awaiting a process reply need not own the UI
                 * source that lets the worker complete it.  A global pump is
                 * still safe: the AROS broker admits only already-bound,
                 * typed destinations, never an ordinary guest mailbox. */
                interactive = global_matched != 0;
                if (global_pumped) pumped = global_pumped;
            }
            /* The window port may belong to a guest worker rather than the
             * context whose Wait brought us here.  Native Intuition has now
             * signalled that port's recorded owner; give that owner a turn
             * before looking only at the current task's signal word. */
            /* A sibling that consumed the previous event may still be in the
             * middle of its handler after one bounded JIT quantum.  Keep
             * scheduling every runnable sibling even when this particular
             * pump found no NEW message; otherwise a long repaint/menu
             * handler is stranded forever waiting for unrelated input. */
            if (emu68k_reschedule_siblings(
                    r, sb, pumped ? "IDCMP delivery" : "runnable continuation",
                    st->pc, e, el) != 0)
                return 1;
            got = gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF) & want;
            if (got) {
                gwrite32(sb, ctx_task(r) + TASK_SIGRECVD_OFF,
                         gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF) & ~got);
                st->d[0] = got;
                return 0;
            }
            if (interactive && emu68k_oscall) {
                bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                         "scheduler.yield", "\"reason\":\"native event wait\"");
                for (;;) {
                    /* Idle through the OS, never through the host thread.
                     *
                     * AROS schedules cooperatively: a task keeps the CPU until
                     * it makes an OS wait. A host sleep parks the thread while
                     * AROS still counts this task as RUNNING, so `cocoa.hidd
                     * input` (pri 50) and `input.device` stay READY and never
                     * run - and the very input this loop is waiting for can
                     * never be produced. That livelock is self-sustaining:
                     * once entered, nothing can wake it. A native Delay blocks
                     * the task on timer.device the way every other hosted idle
                     * task does, so the scheduler runs them.
                     *
                     * WaitTOF is deliberately not used: on a hosted display it
                     * may be a successful no-op, producing a hot loop. */
                    if (emu68k_oscall) {
                        struct j5d_m68k_state idle = *st;
                        char scratch[128] = {0};
                        idle.d[1] = 1;            /* one tick: 1/50 s */
                        (void)emu68k_oscall("dos.library", DOS_LVO_DELAY,
                                            &idle, r->reserve,
                                            emu68k_oscall_user,
                                            scratch, sizeof scratch);
                    } else {
                        /* No OS behind us (standalone host fixtures): the
                         * thread is the only thing there is to yield. */
                        const struct timespec frame = { 0, 16666667L };
                        nanosleep(&frame, NULL);
                    }
                    pumped = event_pump(r, st, 0, UINT32_MAX, NULL);
                    if (emu68k_reschedule_siblings(
                            r, sb,
                            pumped ? "IDCMP delivery" : "runnable continuation",
                            st->pc, e, el) != 0)
                        return 1;
                    got = gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF) & want;
                    if (got) {
                        gwrite32(sb, ctx_task(r) + TASK_SIGRECVD_OFF,
                                 gread32(sb, ctx_task(r) + TASK_SIGRECVD_OFF)
                                 & ~got);
                        st->d[0] = got;
                        bl_event(BL_RUNTIME, r->cur_ctx, ctx_task(r), st->pc,
                                 "scheduler.resume", "\"after\":\"IDCMP\"");
                        return 0;
                    }
                    if (r->kill_req)
                        break;
                    if (r->deadline > 0.0) {
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        if ((double)ts.tv_sec + ts.tv_nsec / 1e9 > r->deadline) {
                            r->kill_req = 1;
                            break;
                        }
                    }
                }
            }
        }
        if (r->kill_req) {
            bl_event(BL_SUMMARY, r->cur_ctx, ctx_task(r), st->pc,
                     "event.wait.interrupted",
                     "\"mask\":\"0x%08x\",\"task\":\"%s\"",
                     want, bl_id("task", ctx_task(r)));
            snprintf(e, el, "Wait($%08lx) interrupted by the run deadline or stop request",
                     (unsigned long)want);
            return 1;
        }
        bl_event(BL_SUMMARY, r->cur_ctx, ctx_task(r), st->pc,
                 "event.wait.unbound",
                 "\"mask\":\"0x%08x\",\"task\":\"%s\"",
                 want, bl_id("task", ctx_task(r)));
        ledger_record(lvo, r->name[0] ? r->name : NULL);
        snprintf(e, el, "capability gap: Wait($%08lx) cannot be satisfied - "
                 "nothing that could send it is able to run",
                 (unsigned long)want);
        return 1;
    }
    case LVO_SIGNAL: {
        uint32_t task = st->a[1], sigs = st->d[0];
        if (task)
            gwrite32(sb, task + TASK_SIGRECVD_OFF,
                     gread32(sb, task + TASK_SIGRECVD_OFF) | sigs);
        return 0;
    }
    case LVO_ALLOCSIGNAL: {
        uint32_t alloc = gread32(sb, ctx_task(r) + TASK_SIGALLOC_OFF);
        int want = (int32_t)st->d[0];
        int bit = -1;
        if (want >= 0 && want < 32) {
            if (!(alloc & (1u << want))) bit = want;
        } else {
            for (bit = 31; bit >= 16; bit--)
                if (!(alloc & (1u << bit))) break;
            if (bit < 16) bit = -1;
        }
        if (bit >= 0)
            gwrite32(sb, ctx_task(r) + TASK_SIGALLOC_OFF,
                     alloc | (1u << bit));
        st->d[0] = (uint32_t)(int32_t)bit;
        return 0;
    }
    case LVO_FREESIGNAL: {
        int bit = (int32_t)st->d[0];
        if (bit >= 0 && bit < 32) {
            uint32_t alloc = gread32(sb, ctx_task(r) + TASK_SIGALLOC_OFF);
            gwrite32(sb, ctx_task(r) + TASK_SIGALLOC_OFF,
                     alloc & ~(1u << bit));
        }
        return 0;
    }
    case LVO_ALLOCTRAP: {
        uint32_t task = ctx_task(r);
        uint32_t trapalloc = guest_trapalloc_addr(sb, task, e, el);
        uint32_t alloc;
        int want = (int32_t)st->d[0], trap = -1;
        if (!trapalloc) return 1;
        alloc = gread16(sb, trapalloc);
        if (want >= 0 && want < 16) {
            if (!(alloc & (1u << want))) trap = want;
        } else if (want < 0) {
            for (trap = 0; trap < 16; trap++)
                if (!(alloc & (1u << trap))) break;
            if (trap == 16) trap = -1;
        }
        if (trap >= 0)
            gwrite16(sb, trapalloc, alloc | (1u << trap));
        st->d[0] = (uint32_t)(int32_t)trap;
        return 0;
    }
    case LVO_FREETRAP: {
        int trap = (int32_t)st->d[0];
        if (trap >= 0 && trap < 16) {
            uint32_t task = ctx_task(r);
            uint32_t trapalloc = guest_trapalloc_addr(sb, task, e, el);
            uint32_t alloc;
            if (!trapalloc) return 1;
            alloc = gread16(sb, trapalloc);
            gwrite16(sb, trapalloc, alloc & ~(1u << trap));
        }
        return 0;
    }
    case LVO_AVL_ADDNODE: {
        uint32_t rootp = st->a[0], node = st->a[1], callback = st->a[2];
        uint32_t *nodes = NULL;
        size_t count = 0, at = 0;
        if (!guest_span_ok(sb, rootp, 4u) || !guest_span_ok(sb, node, 16u)) {
            snprintf(e, el, "AVL_AddNode root or node is outside guest memory");
            return 1;
        }
        if (avl_collect(sb, gread32(sb, rootp), &nodes, &count, e, el) != 0)
            return 1;
        for (; at < count; at++) {
            int32_t cmp;
            if (avl_compare_key(r, callback, nodes[at], node, &cmp, e, el) != 0) {
                free(nodes);
                return 1;
            }
            if (cmp == 0) {
                st->d[0] = nodes[at];
                free(nodes);
                return 0;
            }
            if (cmp > 0) break;
        }
        {
            uint32_t *grown = realloc(nodes, (count + 1u) * sizeof *nodes);
            if (!grown) {
                free(nodes);
                snprintf(e, el, "host memory exhausted adding AVL node");
                return 1;
            }
            nodes = grown;
        }
        memmove(&nodes[at + 1u], &nodes[at], (count - at) * sizeof *nodes);
        nodes[at] = node;
        avl_rebuild(sb, rootp, nodes, count + 1u);
        free(nodes);
        st->d[0] = 0;
        return 0;
    }
    case LVO_AVL_REMNODEBYADDRESS:
    case LVO_AVL_REMNODEBYKEY: {
        uint32_t rootp = st->a[0], wanted = st->a[1], callback = st->a[2];
        uint32_t *nodes = NULL, removed = 0;
        size_t count = 0, at;
        if (!guest_span_ok(sb, rootp, 4u)) {
            snprintf(e, el, "AVL remove root pointer is outside guest memory");
            return 1;
        }
        if (avl_collect(sb, gread32(sb, rootp), &nodes, &count, e, el) != 0)
            return 1;
        for (at = 0; at < count; at++) {
            if (lvo == LVO_AVL_REMNODEBYADDRESS) {
                if (nodes[at] == wanted) { removed = nodes[at]; break; }
            } else {
                int32_t cmp;
                if (avl_compare_key(r, callback, nodes[at], wanted, &cmp, e, el) != 0) {
                    free(nodes);
                    return 1;
                }
                if (cmp == 0) { removed = nodes[at]; break; }
                if (cmp > 0) break;
            }
        }
        if (removed) {
            memmove(&nodes[at], &nodes[at + 1u],
                    (count - at - 1u) * sizeof *nodes);
            avl_rebuild(sb, rootp, nodes, count - 1u);
            gwrite32(sb, removed + AVL_LEFT_OFF, 0);
            gwrite32(sb, removed + AVL_RIGHT_OFF, 0);
            gwrite32(sb, removed + AVL_PARENT_OFF, 0);
            gwrite32(sb, removed + 12u, 0);
        }
        free(nodes);
        st->d[0] = removed;
        return 0;
    }
    case LVO_AVL_FINDFIRSTNODE:
    case LVO_AVL_FINDLASTNODE: {
        uint32_t out = avl_extreme(sb, st->a[0],
                                   lvo == LVO_AVL_FINDLASTNODE, e, el);
        if (out == UINT32_MAX) return 1;
        st->d[0] = out;
        return 0;
    }
    case LVO_AVL_FINDPREVNODEBYADDRESS:
    case LVO_AVL_FINDNEXTNODEBYADDRESS: {
        uint32_t out = avl_adjacent(sb, st->a[0],
                                    lvo == LVO_AVL_FINDNEXTNODEBYADDRESS, e, el);
        if (out == UINT32_MAX) return 1;
        st->d[0] = out;
        return 0;
    }
    case LVO_AVL_FINDNODE:
    case LVO_AVL_FINDPREVNODEBYKEY:
    case LVO_AVL_FINDNEXTNODEBYKEY: {
        uint32_t node = st->a[0], key = st->a[1], callback = st->a[2];
        uint32_t candidate = 0;
        unsigned guard = 0;
        while (node) {
            int32_t cmp;
            if (!guest_span_ok(sb, node, 16u)) {
                snprintf(e, el, "AVL node %08x is outside guest memory", node);
                return 1;
            }
            if (avl_compare_key(r, callback, node, key, &cmp, e, el) != 0)
                return 1;
            if (cmp == 0) {
                st->d[0] = node;
                return 0;
            }
            if (lvo == LVO_AVL_FINDPREVNODEBYKEY && cmp < 0) candidate = node;
            if (lvo == LVO_AVL_FINDNEXTNODEBYKEY && cmp > 0) candidate = node;
            node = gread32(sb, node + (cmp < 0 ? AVL_RIGHT_OFF : AVL_LEFT_OFF));
            if (++guard > 65536u) {
                snprintf(e, el, "AVL tree exceeds 65536 nodes or contains a cycle");
                return 1;
            }
        }
        st->d[0] = candidate;
        return 0;
    }
    case LVO_STACKSWAP: {
        /* struct StackSwapStruct is three guest pointers: lower, upper and
         * switch-point SP. Swap those values here; a second call restores the
         * original stack, just like exec.library on a real 68k system. */
        uint32_t sss = st->a[0];
        uint32_t new_lower, new_upper, new_sp, old_sp = st->a[7];
        if (!sss || sss < sb->sandbox_origin ||
            (uint64_t)sss + 12u > (uint64_t)sb->sandbox_origin + sb->size) {
            snprintf(e, el, "StackSwap structure A0=%08x is outside guest memory", sss);
            return 1;
        }
        new_lower = gread32(sb, sss);
        new_upper = gread32(sb, sss + 4u);
        new_sp = gread32(sb, sss + 8u);
        if (new_lower < sb->sandbox_origin || new_upper <= new_lower ||
            new_sp < new_lower || new_sp > new_upper ||
            (uint64_t)new_upper > (uint64_t)sb->sandbox_origin + sb->size) {
            snprintf(e, el, "StackSwap requested invalid range %08x..%08x sp=%08x",
                     new_lower, new_upper, new_sp);
            return 1;
        }
        gwrite32(sb, sss, r->stack_lower);
        gwrite32(sb, sss + 4u, r->stack_upper);
        gwrite32(sb, sss + 8u, old_sp);
        r->stack_lower = new_lower;
        r->stack_upper = new_upper;
        st->a[7] = new_sp;
        gwrite32(sb, GUEST_PROCESS + TASK_SPREG_OFF, new_sp);
        gwrite32(sb, GUEST_PROCESS + TASK_SPLOWER_OFF, new_lower);
        gwrite32(sb, GUEST_PROCESS + TASK_SPUPPER_OFF, new_upper);
        return 0;
    }
    case LVO_NEWSTACKSWAP: {
        uint32_t sss = st->a[0], entry = st->a[1], args = st->a[2];
        uint32_t lower, upper, requested_sp, call_sp, result = 0;
        uint32_t task = ctx_task(r);
        uint32_t old_reg, old_lower, old_upper;
        struct j5d_m68k_state call;
        int rc;
        if (!guest_span_ok(sb, sss, 12u) || !guest_span_ok(sb, entry, 2u)) {
            snprintf(e, el, "NewStackSwap structure or entry is outside guest memory");
            return 1;
        }
        lower = gread32(sb, sss);
        upper = gread32(sb, sss + 4u);
        requested_sp = gread32(sb, sss + 8u) & ~1u;
        if (lower < sb->sandbox_origin || upper <= lower ||
            requested_sp < lower || requested_sp > upper ||
            (uint64_t)upper > (uint64_t)sb->sandbox_origin + sb->size) {
            snprintf(e, el, "NewStackSwap requested invalid range %08x..%08x sp=%08x",
                     lower, upper, requested_sp);
            return 1;
        }
        if (args && !guest_span_ok(sb, args, 32u)) {
            snprintf(e, el, "NewStackSwap arguments %08x are outside guest memory", args);
            return 1;
        }
        if (requested_sp < lower + (args ? 36u : 4u)) {
            snprintf(e, el, "NewStackSwap has no room for its return and arguments");
            return 1;
        }
        call_sp = requested_sp - (args ? 36u : 4u);
        gwrite32(sb, call_sp, 0);                 /* direct-run RTS sentinel */
        if (args)
            for (unsigned i = 0; i < 8; i++)
                gwrite32(sb, call_sp + 4u + i * 4u, gread32(sb, args + i * 4u));
        memset(&call, 0, sizeof call);
        call.a[6] = EXEC_BASE;
        old_reg = gread32(sb, task + TASK_SPREG_OFF);
        old_lower = gread32(sb, task + TASK_SPLOWER_OFF);
        old_upper = gread32(sb, task + TASK_SPUPPER_OFF);
        gwrite32(sb, task + TASK_SPREG_OFF, requested_sp);
        gwrite32(sb, task + TASK_SPLOWER_OFF, lower);
        gwrite32(sb, task + TASK_SPUPPER_OFF, upper);
        rc = run_guest_subroutine(r, entry, &call, call_sp, &result, e, el);
        gwrite32(sb, task + TASK_SPREG_OFF, old_reg);
        gwrite32(sb, task + TASK_SPLOWER_OFF, old_lower);
        gwrite32(sb, task + TASK_SPUPPER_OFF, old_upper);
        if (rc != 0) return 1;
        st->d[0] = result;
        return 0;
    }
    default:
        if (emu68k_host_getenv("EMU68K_DEBUG_EXEC"))
            fprintf(stderr, "[exec_call] unhandled lvo=%d (ADDHEAD=%d)\n",
                    lvo, LVO_ADDHEAD);
        return 1;                                        /* not served here     */
    }
}
