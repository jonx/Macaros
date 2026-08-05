/* Guest-side task.resource semantics for the hosted 68k bridge.
 *
 * The native resource cannot own these values: Hook pointers, task identities
 * and IPTR task-storage values are all addresses in the 32-bit guest arena.
 * Keep that state per emu68k run and execute guest dispatchers in the guest.
 */
#include "emu68k_internal.h"
#include "emu68k_guest_offsets.h"

#include <stdio.h>
#include <string.h>

#define TASK_LVO_LOCK_LIST          1
#define TASK_LVO_UNLOCK_LIST        2
#define TASK_LVO_NEXT_ENTRY         3
#define TASK_LVO_QUERY_TAGS         6
#define TASK_LVO_INIT_HOOKS         9
#define TASK_LVO_ADD_HOOK          10
#define TASK_LVO_RUN_HOOKS         11
#define TASK_LVO_ALLOC_SLOT        12
#define TASK_LVO_FREE_SLOT         13
#define TASK_LVO_SAVE_STORAGE      14
#define TASK_LVO_RESTORE_STORAGE   15
#define TASK_LVO_SET_SLOT          16
#define TASK_LVO_GET_SLOT          17
#define TASK_LVO_GET_PARENT_SLOT   18

#define THF_ROA 1u
#define THF_IAR 2u
#define TASK_STORAGE_MAGIC 0x54534c53u /* "TSLS" */

static int guest_span_ok(j4_sandbox *sb, uint32_t addr, uint32_t size)
{
    return addr >= sb->sandbox_origin &&
           (uint64_t)addr + size <=
               (uint64_t)sb->sandbox_origin + sb->size;
}

static int hook_type_slot(struct emu68k_run *r, uint32_t task, uint32_t type,
                          int create)
{
    int free_slot = -1;
    for (int i = 0; i < EMU68K_TASK_HOOK_TYPES_MAX; i++) {
        if (r->task_hook_type[i].live &&
            r->task_hook_type[i].task == task &&
            r->task_hook_type[i].type == type)
            return i;
        if (free_slot < 0 && !r->task_hook_type[i].live) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    memset(&r->task_hook_type[free_slot], 0,
           sizeof r->task_hook_type[free_slot]);
    r->task_hook_type[free_slot].task = task;
    r->task_hook_type[free_slot].type = type;
    r->task_hook_type[free_slot].live = 1;
    return free_slot;
}

/* TaskResHookDispatcher is an ordinary C callback, not the register-based
 * Hook ABI.  At entry its Hook * argument is 4(sp), after the return address.
 * j5d_run treats an RTS at the initial stack depth as callback completion, so
 * the placeholder return word need not be executable. */
static int run_dispatcher(struct emu68k_run *r, j4_sandbox *sb,
                          uint32_t dispatcher, uint32_t hook,
                          uint32_t *result, char *e, unsigned el)
{
    struct j5d_m68k_state call;
    uint32_t stack_top, sp;
    if (!dispatcher || !guest_span_ok(sb, dispatcher, 2u)) {
        snprintf(e, el, "task.resource dispatcher %08x is not guest code",
                 dispatcher);
        return 1;
    }
    stack_top = emu68k_callback_stack_acquire(r, e, el);
    if (!stack_top) return 1;
    sp = stack_top - 8u;
    gwrite32(sb, sp, 0);
    gwrite32(sb, sp + 4u, hook);
    memset(&call, 0, sizeof call);
    call.a[7] = sp;
    {
        int rc = run_guest_subroutine(r, dispatcher, &call, sp, result, e, el);
        emu68k_callback_stack_release(r);
        return rc;
    }
}

static int storage_find(struct emu68k_run *r, uint32_t task, uint32_t slot)
{
    for (int i = 0; i < EMU68K_TASK_STORAGE_MAX; i++)
        if (r->task_storage[i].live && r->task_storage[i].task == task &&
            r->task_storage[i].slot == slot)
            return i;
    return -1;
}

static int storage_set(struct emu68k_run *r, uint32_t task, uint32_t slot,
                       uint32_t value)
{
    int i = storage_find(r, task, slot);
    if (i < 0) {
        for (i = 0; i < EMU68K_TASK_STORAGE_MAX; i++)
            if (!r->task_storage[i].live) break;
        if (i == EMU68K_TASK_STORAGE_MAX) return 1;
        r->task_storage[i].task = task;
        r->task_storage[i].slot = slot;
        r->task_storage[i].live = 1;
    }
    r->task_storage[i].value = value;
    return 0;
}

int emu68k_taskresource_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                             struct j5d_m68k_state *st, char *e, unsigned el)
{
    uint32_t task = ctx_task(r);
    int idx;

    switch (lvo) {
    case TASK_LVO_INIT_HOOKS:
        idx = hook_type_slot(r, task, st->d[0], 1);
        if (idx < 0) {
            snprintf(e, el, "task.resource hook-type table is full");
            return 1;
        }
        if (!r->task_hook_type[idx].dispatcher)
            r->task_hook_type[idx].dispatcher = st->a[0];
        r->task_hook_type[idx].flags = (uint8_t)st->d[1];
        st->d[0] = 1;
        return 0;

    case TASK_LVO_ADD_HOOK: {
        uint32_t result = 1;
        idx = hook_type_slot(r, task, st->d[0], 1);
        if (idx < 0) {
            snprintf(e, el, "task.resource hook-type table is full");
            return 1;
        }
        if (!guest_span_ok(sb, st->a[0], M68K_Hook_SIZEOF)) {
            snprintf(e, el, "task.resource Hook %08x is outside guest memory",
                     st->a[0]);
            return 1;
        }
        if ((r->task_hook_type[idx].flags == THF_ROA) ||
            (r->task_hook_type[idx].flags == THF_IAR &&
             r->task_hook_type[idx].ran)) {
            if (run_dispatcher(r, sb, r->task_hook_type[idx].dispatcher,
                               st->a[0], &result, e, el) != 0)
                return 1;
            st->d[0] = result != 0;
            return 0;
        }
        if (r->task_hook_type[idx].hook_count >= EMU68K_TASK_HOOKS_MAX) {
            snprintf(e, el, "task.resource hook list is full");
            return 1;
        }
        r->task_hook_type[idx].hook[r->task_hook_type[idx].hook_count++] =
            st->a[0];
        st->d[0] = 1;
        return 0;
    }

    case TASK_LVO_RUN_HOOKS:
        idx = hook_type_slot(r, task, st->d[0], 0);
        if (idx < 0) { st->d[0] = 0; return 0; }
        for (unsigned i = 0; i < r->task_hook_type[idx].hook_count; i++) {
            uint32_t result = 0;
            if (run_dispatcher(r, sb, st->a[0],
                               r->task_hook_type[idx].hook[i],
                               &result, e, el) != 0)
                return 1;
            if (!result) { st->d[0] = 0; return 0; }
        }
        r->task_hook_type[idx].ran = 1;
        st->d[0] = 1;
        return 0;

    case TASK_LVO_ALLOC_SLOT:
        if (!r->next_task_storage_slot) r->next_task_storage_slot = 1;
        if (r->next_task_storage_slot == UINT32_MAX) {
            st->d[0] = 0;
            return 0;
        }
        st->d[0] = r->next_task_storage_slot++;
        return 0;

    case TASK_LVO_FREE_SLOT:
        for (int i = 0; i < EMU68K_TASK_STORAGE_MAX; i++)
            if (r->task_storage[i].live &&
                r->task_storage[i].slot == st->d[0])
                r->task_storage[i].live = 0;
        return 0;

    case TASK_LVO_SET_SLOT:
        if (!st->d[0]) { st->d[0] = 0; return 0; }
        st->d[0] = storage_set(r, task, st->d[0], st->d[1]) == 0;
        return 0;

    case TASK_LVO_GET_SLOT:
        idx = storage_find(r, task, st->d[0]);
        st->d[0] = idx >= 0 ? r->task_storage[idx].value : 0;
        return 0;

    case TASK_LVO_GET_PARENT_SLOT:
        /* Guest task contexts do not currently retain a parent identity. */
        st->d[0] = 0;
        return 0;

    case TASK_LVO_SAVE_STORAGE: {
        uint32_t count = 0, out, p;
        for (int i = 0; i < EMU68K_TASK_STORAGE_MAX; i++)
            if (r->task_storage[i].live && r->task_storage[i].task == task)
                count++;
        if (!count) { st->d[0] = 0; return 0; }
        out = guest_alloc(r, 12u + count * 8u);
        if (!out) { st->d[0] = 0; return 0; }
        gwrite32(sb, out, TASK_STORAGE_MAGIC);
        gwrite32(sb, out + 4u, task);
        gwrite32(sb, out + 8u, count);
        p = out + 12u;
        for (int i = 0; i < EMU68K_TASK_STORAGE_MAX; i++) {
            if (!r->task_storage[i].live || r->task_storage[i].task != task)
                continue;
            gwrite32(sb, p, r->task_storage[i].slot);
            gwrite32(sb, p + 4u, r->task_storage[i].value);
            p += 8u;
        }
        st->d[0] = out;
        return 0;
    }

    case TASK_LVO_RESTORE_STORAGE: {
        uint32_t handle = st->a[0], count;
        if (!handle) return 0;
        if (!guest_span_ok(sb, handle, 12u) ||
            gread32(sb, handle) != TASK_STORAGE_MAGIC) {
            snprintf(e, el, "task.resource storage handle %08x is invalid",
                     handle);
            return 1;
        }
        count = gread32(sb, handle + 8u);
        if (count > EMU68K_TASK_STORAGE_MAX ||
            !guest_span_ok(sb, handle, 12u + count * 8u)) {
            snprintf(e, el, "task.resource storage snapshot is malformed");
            return 1;
        }
        for (int i = 0; i < EMU68K_TASK_STORAGE_MAX; i++)
            if (r->task_storage[i].live && r->task_storage[i].task == task)
                r->task_storage[i].live = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t p = handle + 12u + i * 8u;
            if (storage_set(r, task, gread32(sb, p),
                            gread32(sb, p + 4u)) != 0) {
                snprintf(e, el, "task.resource storage table is full");
                return 1;
            }
        }
        return 0;
    }

    case TASK_LVO_LOCK_LIST:
    case TASK_LVO_UNLOCK_LIST:
    case TASK_LVO_NEXT_ENTRY:
    case TASK_LVO_QUERY_TAGS:
        snprintf(e, el, "task.resource function LVO %d requires guest task-list "
                        "inspection, which is not implemented", lvo);
        return 1;
    default:
        return 1;
    }
}
