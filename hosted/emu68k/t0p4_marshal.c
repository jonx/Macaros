/* t0p4_marshal.c — [T0-P4] the marshalling-schema spike.
 * (OURS, AROS-licensed. Contains NO Emu68 source; links the engine via libjit68k.)
 *
 * THE QUESTION (docs/features/68k-transparent-exec/plan.md [T0-P4], design.md §4):
 * `.conf`-generated LVO tables give registers and scalar/pointer kinds, but real
 * marshalling needs SEMANTICS the signatures do not carry: pointer direction,
 * buffer-length coupling, natively-traversed structures (guest layout != host
 * layout), opaque handles, native->68k callbacks, and per-tag taglist kinds. This
 * spike defines a candidate ANNOTATION VOCABULARY + a generic descriptor-driven
 * marshaller, and proves it on the plan's five representative cases:
 *
 *   1. BUFFER+LENGTH  (`Read`-class)   — guest buffer written by the native side,
 *      length coupled from another register; out-of-sandbox length = clean fault.
 *   2. SHADOW STRUCT  (`PutMsg`-class) — the native side TRAVERSES a structure: the
 *      marshaller builds a host-layout shadow from the big-endian guest fields by a
 *      field map, and copies OUT-fields back after the call.
 *   3. OPAQUE HANDLE                    — a token the guest must return verbatim;
 *      validated against a handle table, never dereferenced as guest memory.
 *   4. CALLBACK HOOK                    — the native side calls BACK into 68k code:
 *      the marshaller re-enters the engine (nested j5d_run on the same instance,
 *      Amiga hook ABI A0=hook/A2=object/A1=msg, result D0). Driven END-TO-END from
 *      a real 68k caller through the jsr-d16(A6) LVO bridge.
 *   5. TAGLIST                          — per-domain tag kinds (scalar / guest ptr);
 *      an UNKNOWN tag ABORTS the call as a classified capability gap (never
 *      pass-as-scalar), with a ledger record — the design.md §4 rule, executed.
 *
 * VERDICT SHAPE: marshal rc 0 = OK; MARSH_EGAP = capability gap (unknown LVO/tag),
 * ledger-recorded; MARSH_EFAULT = guest-pointer/bounds violation, clean error.
 * Marker: [T0P4] PASS / FAIL. */

#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j3_jit68k.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x01000000u
#define LIBBASE         0x00230000u
#define PROG_ORIGIN     0x00250000u

#define CHECK(cond, why) do { if (!(cond)) { \
    fprintf(stderr, "[T0P4] FAIL: %s (line %d)\n", why, __LINE__); exit(1); } } while (0)

/* ====================== the annotation vocabulary (the schema) ===================== */

typedef enum {                    /* what a 68k register argument MEANS               */
    ARG_U32 = 0,                  /* scalar, width-handled                             */
    ARG_PTR_IN,                   /* guest ptr, native READS len bytes (len_arg)       */
    ARG_PTR_OUT,                  /* guest ptr, native WRITES len bytes (len_arg)      */
    ARG_CSTR_IN,                  /* guest ptr to NUL-terminated string (bounded scan) */
    ARG_HANDLE,                   /* opaque token: validate in the table, never deref  */
    ARG_SHADOW,                   /* struct traversed natively: build host shadow via
                                     the field map (shadow_cls), copy OUT-fields back  */
    ARG_HOOK,                     /* guest code addr: native may call back into 68k    */
    ARG_TAGLIST                   /* guest TagItem list, per-domain kinds (tag_dom)    */
} arg_kind;

typedef struct {                  /* one field of a shadow class                       */
    uint16_t guest_off;           /* offset in the BE guest struct                     */
    uint8_t  size;                /* 1/2/4                                             */
    uint8_t  dir;                 /* 0=IN (guest->host), 1=OUT (host->guest after)     */
    uint8_t  is_ptr;              /* IN only: translate guest ptr -> host ptr          */
    uint16_t host_off;            /* offset in the host shadow struct                  */
} shadow_field;

typedef struct {                  /* a shadow class = field map + sizes                */
    const shadow_field *fields; int nfields;
    uint16_t guest_size; uint16_t host_size;
} shadow_class;

typedef struct { uint32_t tag; uint8_t is_ptr; } tag_desc;   /* one known tag         */
typedef struct { const tag_desc *tags; int ntags; } tag_domain;

typedef struct {                  /* one argument slot                                 */
    uint8_t  reg;                 /* 0..7 = D0..D7, 8..14 = A0..A6                     */
    uint8_t  kind;                /* arg_kind                                          */
    int8_t   len_arg;             /* PTR_IN/OUT: index of the U32 arg holding length   */
    int8_t   aux;                 /* SHADOW: shadow_cls idx · TAGLIST: tag_dom idx     */
} arg_desc;

typedef struct {                  /* one function: the lvo_desc + annotations          */
    int         lvo;              /* positive LVO index (n; offset -6n)                */
    uint8_t     ret;              /* 0=void, 1=u32 -> D0                               */
    uint8_t     nargs;
    arg_desc    arg[6];
    int (*native)(uint64_t *hv, uint32_t *ret, void *ctx, char *err, unsigned el);
} fn_desc;                        /* hv[i] = marshalled host value of arg i            */

#define MARSH_EGAP   200          /* classified capability gap (unknown LVO / tag)     */
#define MARSH_EFAULT 201          /* guest pointer/bounds violation                    */

/* the capability-gap ledger (the design.md "crashes as fuel" object, spike-sized) */
static struct { int lvo; uint32_t tag; } g_ledger[16];
static int g_ledger_n = 0;

/* ============================ the generic marshaller =============================== */

typedef struct {
    j4_sandbox   *sb;
    j5d_engine   *eng;            /* for nested hook re-entry                          */
    uint32_t      hook_sp;        /* 68k SP to run hooks on                            */
    /* the handle table */
    uint32_t      handles[8]; int nhandles;
    void         *ctx;            /* native-side context                               */
} marsh_env;

static uint32_t reg_get(const struct j5d_m68k_state *st, uint8_t r)
{ return r < 8 ? st->d[r] : st->a[r - 8]; }

static uint8_t *guest_range(marsh_env *m, uint32_t addr, uint32_t len)
{
    if (addr < m->sb->sandbox_origin ||
        (uint64_t)addr + len > (uint64_t)m->sb->sandbox_origin + m->sb->size)
        return NULL;
    return j4_sandbox_host(m->sb, addr);
}
static uint32_t gbe32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t gbe16(const uint8_t *p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static void pbe32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }

/* run a 68k hook: nested engine entry, Amiga hook ABI (A0=hook, A2=object, A1=msg),
 * result = D0. The hook runs on its own SP region; the engine instance is shared. */
static int call_hook_68k(marsh_env *m, uint32_t hook_pc, uint32_t a1_msg, uint32_t a2_obj,
                         uint32_t *d0_out, char *err, unsigned errlen)
{
    struct j5d_m68k_state hst; memset(&hst, 0, sizeof hst);
    hst.a[0] = hook_pc; hst.a[1] = a1_msg; hst.a[2] = a2_obj;
    hst.a[7] = m->hook_sp;
    j5d_sandbox sb = { m->sb->host_mem, m->sb->sandbox_origin, m->sb->size };
    return j5d_run(&sb, hook_pc, LIBBASE, &hst, d0_out, NULL, NULL, err, errlen);
}

/* the descriptor-driven call: marshal per annotation, invoke, copy back, return. */
static int marshal_call(const fn_desc *f, struct j5d_m68k_state *st, marsh_env *m,
                        char *err, unsigned errlen)
{
    uint64_t hv[6] = {0};
    uint8_t  shadow_buf[64];
    int      shadow_arg = -1;
    uint32_t shadow_gaddr = 0;
    extern const shadow_class g_shadow_cls[];
    extern const tag_domain   g_tag_dom[];

    /* TWO PASSES, a finding this spike caught the hard way: a buffer's length may
     * live in an argument DECLARED AFTER the buffer, so single-pass in-order
     * marshalling bounds-checks against length 0 and lets the native side write past
     * the arena. Scalars (the only things lengths can be) marshal in pass 0; every
     * pointer-ish kind marshals in pass 1, when all lengths are in hv[]. */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < f->nargs; i++) {
        const arg_desc *a = &f->arg[i];
        int scalar = (a->kind == ARG_U32 || a->kind == ARG_HANDLE);
        if ((pass == 0) != scalar) continue;
        uint32_t rv = reg_get(st, a->reg);
        switch ((arg_kind)a->kind) {
        case ARG_U32:    hv[i] = rv; break;
        case ARG_HANDLE: {
            int ok = 0;
            for (int h = 0; h < m->nhandles; h++) if (m->handles[h] == rv) ok = 1;
            if (!ok) { snprintf(err, errlen, "bad handle %08x", rv); return MARSH_EFAULT; }
            hv[i] = rv; break;
        }
        case ARG_PTR_IN: case ARG_PTR_OUT: {
            uint32_t len = (uint32_t)hv[a->len_arg];      /* length arg marshals first */
            uint8_t *hp = guest_range(m, rv, len);
            if (!hp) { snprintf(err, errlen, "guest buffer %08x+%u out of sandbox", rv, len);
                       return MARSH_EFAULT; }
            hv[i] = (uint64_t)(uintptr_t)hp; break;
        }
        case ARG_CSTR_IN: {
            uint8_t *hp = guest_range(m, rv, 1);
            if (!hp) { snprintf(err, errlen, "guest cstr %08x out of sandbox", rv);
                       return MARSH_EFAULT; }
            uint32_t max = m->sb->sandbox_origin + m->sb->size - rv;
            if (!memchr(hp, 0, max)) { snprintf(err, errlen, "unterminated cstr"); return MARSH_EFAULT; }
            hv[i] = (uint64_t)(uintptr_t)hp; break;
        }
        case ARG_SHADOW: {
            const shadow_class *sc = &g_shadow_cls[a->aux];
            uint8_t *gp = guest_range(m, rv, sc->guest_size);
            if (!gp) { snprintf(err, errlen, "guest struct %08x out of sandbox", rv);
                       return MARSH_EFAULT; }
            memset(shadow_buf, 0, sizeof shadow_buf);
            for (int k = 0; k < sc->nfields; k++) {
                const shadow_field *sf = &sc->fields[k];
                if (sf->dir != 0) continue;               /* IN fields only, here      */
                uint32_t v = sf->size == 4 ? gbe32(gp + sf->guest_off)
                           : sf->size == 2 ? gbe16(gp + sf->guest_off)
                           : gp[sf->guest_off];
                if (sf->is_ptr) {
                    uint8_t *fp = guest_range(m, v, 1);
                    if (!fp) { snprintf(err, errlen, "guest field ptr %08x bad", v);
                               return MARSH_EFAULT; }
                    *(uint8_t **)(shadow_buf + sf->host_off) = fp;
                } else if (sf->size == 2) {
                    *(uint16_t *)(shadow_buf + sf->host_off) = (uint16_t)v;
                } else {
                    *(uint32_t *)(shadow_buf + sf->host_off) = v;
                }
            }
            shadow_arg = i; shadow_gaddr = rv;
            hv[i] = (uint64_t)(uintptr_t)shadow_buf; break;
        }
        case ARG_HOOK: {
            if (!guest_range(m, rv, 2)) { snprintf(err, errlen, "hook pc %08x bad", rv);
                                          return MARSH_EFAULT; }
            hv[i] = rv;                                    /* native calls it via env  */
            break;
        }
        case ARG_TAGLIST: {
            const tag_domain *dom = &g_tag_dom[a->aux];
            uint32_t ti = rv;
            for (;;) {
                uint8_t *tp = guest_range(m, ti, 8);
                if (!tp) { snprintf(err, errlen, "taglist %08x out of sandbox", ti);
                           return MARSH_EFAULT; }
                uint32_t tag = gbe32(tp), data = gbe32(tp + 4);
                if (tag == 0) break;                       /* TAG_DONE                 */
                const tag_desc *td = NULL;
                for (int k = 0; k < dom->ntags; k++)
                    if (dom->tags[k].tag == tag) td = &dom->tags[k];
                if (!td) {
                    /* THE RULE (design.md §4): unknown tag = classified capability
                     * gap + ledger record. Never a guess, never pass-as-scalar. */
                    if (g_ledger_n < 16) { g_ledger[g_ledger_n].lvo = f->lvo;
                                           g_ledger[g_ledger_n].tag = tag; g_ledger_n++; }
                    snprintf(err, errlen, "capability gap: unknown tag %08x (lvo %d)",
                             tag, f->lvo);
                    return MARSH_EGAP;
                }
                if (td->is_ptr && !guest_range(m, data, 1)) {
                    snprintf(err, errlen, "tag %08x ptr data %08x bad", tag, data);
                    return MARSH_EFAULT;
                }
                (void)data;
                ti += 8;
            }
            hv[i] = rv; break;                             /* native re-walks via env  */
        }
        }
    }

    uint32_t ret = 0;
    int rc = f->native(hv, &ret, m, err, errlen);
    if (rc) return rc;

    /* copy-back: OUT fields of the shadow struct return to guest memory, big-endian */
    if (shadow_arg >= 0) {
        const shadow_class *sc = &g_shadow_cls[f->arg[shadow_arg].aux];
        uint8_t *gp = guest_range(m, shadow_gaddr, sc->guest_size);
        for (int k = 0; k < sc->nfields; k++) {
            const shadow_field *sf = &sc->fields[k];
            if (sf->dir != 1) continue;
            uint32_t v = *(uint32_t *)(shadow_buf + sf->host_off);
            if (sf->size == 4) pbe32(gp + sf->guest_off, v);
            else if (sf->size == 2) { gp[sf->guest_off] = (uint8_t)(v >> 8);
                                      gp[sf->guest_off + 1] = (uint8_t)v; }
            else gp[sf->guest_off] = (uint8_t)v;
        }
    }
    if (f->ret == 1) st->d[0] = ret;
    return 0;
}

/* ========================= the spike library (5 natives) =========================== */

/* the guest "SpikeMsg" struct: { name BPTR u32 @0; length u16 @4; payload u32 @6;
 * reply u32 @10 } — 14 bytes, unaligned u32s on purpose (BE guest layout is not host
 * layout; that is the point of the shadow). Host shadow: */
struct spike_msg_host { uint8_t *name; uint16_t length; uint32_t payload; uint32_t reply; };
static const shadow_field msg_fields[] = {
    {  0, 4, 0, 1, offsetof(struct spike_msg_host, name)    },
    {  4, 2, 0, 0, offsetof(struct spike_msg_host, length)  },
    {  6, 4, 0, 0, offsetof(struct spike_msg_host, payload) },
    { 10, 4, 1, 0, offsetof(struct spike_msg_host, reply)   },   /* OUT: copied back  */
};
const shadow_class g_shadow_cls[] = {
    { msg_fields, 4, 14, sizeof(struct spike_msg_host) },
};

#define TAG_WIDTH 0x80000001u
#define TAG_NAME  0x80000002u
static const tag_desc setattrs_tags[] = { { TAG_WIDTH, 0 }, { TAG_NAME, 1 } };
const tag_domain g_tag_dom[] = { { setattrs_tags, 2 } };

/* native state the natives record into (the asserts read it) */
static struct {
    uint32_t read_len;
    char     putmsg_name[32]; uint16_t putmsg_length; uint32_t putmsg_payload;
    uint32_t open_count;
    uint32_t attrs_width; char attrs_name[32];
} g_nat;

static int nat_read(uint64_t *hv, uint32_t *ret, void *ctx, char *err, unsigned el)
{   /* SpikeRead(handle D0, buf A0 OUT, len D1) -> len : fills buf with i^0x5A */
    (void)ctx; (void)err; (void)el;
    uint8_t *buf = (uint8_t *)(uintptr_t)hv[1];
    uint32_t len = (uint32_t)hv[2];
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(i ^ 0x5A);
    g_nat.read_len = len; *ret = len; return 0;
}
static int nat_putmsg(uint64_t *hv, uint32_t *ret, void *ctx, char *err, unsigned el)
{   /* SpikePutMsg(handle D0, msg A0 SHADOW) — native TRAVERSES the host shadow */
    (void)ctx; (void)err; (void)el;
    struct spike_msg_host *msg = (struct spike_msg_host *)(uintptr_t)hv[1];
    snprintf(g_nat.putmsg_name, sizeof g_nat.putmsg_name, "%s", (char *)msg->name);
    g_nat.putmsg_length  = msg->length;
    g_nat.putmsg_payload = msg->payload;
    msg->reply = msg->payload + 1;                 /* the OUT field the guest reads   */
    *ret = 0; return 0;
}
static int nat_open(uint64_t *hv, uint32_t *ret, void *ctx, char *err, unsigned el)
{   /* SpikeOpen(name A0 CSTR) -> HANDLE */
    (void)err; (void)el;
    marsh_env *m = (marsh_env *)ctx;
    (void)hv;
    uint32_t h = 0xBEEF0000u + (uint32_t)m->nhandles;
    m->handles[m->nhandles++] = h;
    g_nat.open_count++; *ret = h; return 0;
}
static int nat_applyhook(uint64_t *hv, uint32_t *ret, void *ctx, char *err, unsigned el)
{   /* SpikeApplyHook(hook A0, msgaddr A1) -> hook's D0 : the native->68k re-entry */
    marsh_env *m = (marsh_env *)ctx;
    uint32_t d0 = 0;
    int rc = call_hook_68k(m, (uint32_t)hv[0], (uint32_t)hv[1], 0, &d0, err, el);
    if (rc) return rc;
    *ret = d0; return 0;
}
static int nat_setattrs(uint64_t *hv, uint32_t *ret, void *ctx, char *err, unsigned el)
{   /* SpikeSetAttrs(taglist A0) — re-walks the (validated) guest list */
    (void)err; (void)el;
    marsh_env *m = (marsh_env *)ctx;
    uint32_t ti = (uint32_t)hv[0];
    for (;;) {
        uint8_t *tp = j4_sandbox_host(m->sb, ti);
        uint32_t tag = gbe32(tp), data = gbe32(tp + 4);
        if (tag == 0) break;
        if (tag == TAG_WIDTH) g_nat.attrs_width = data;
        if (tag == TAG_NAME)
            snprintf(g_nat.attrs_name, sizeof g_nat.attrs_name, "%s",
                     (char *)j4_sandbox_host(m->sb, data));
        ti += 8;
    }
    *ret = 0; return 0;
}

static const fn_desc g_fns[] = {
    { 1, 1, 3, { {0, ARG_HANDLE, -1, 0}, {8, ARG_PTR_OUT, 2, 0}, {1, ARG_U32, -1, 0} }, nat_read },
    { 2, 1, 2, { {0, ARG_HANDLE, -1, 0}, {8, ARG_SHADOW, -1, 0} },                      nat_putmsg },
    { 3, 1, 1, { {8, ARG_CSTR_IN, -1, 0} },                                             nat_open },
    { 4, 1, 2, { {8, ARG_HOOK, -1, 0}, {9, ARG_U32, -1, 0} },                           nat_applyhook },
    { 5, 1, 1, { {8, ARG_TAGLIST, -1, 0} },                                             nat_setattrs },
};

static int spike_dispatch(int lvo, struct j5d_m68k_state *st, void *user,
                          char *err, unsigned errlen)
{
    marsh_env *m = (marsh_env *)user;
    for (unsigned i = 0; i < sizeof g_fns / sizeof g_fns[0]; i++)
        if (g_fns[i].lvo == lvo)
            return marshal_call(&g_fns[i], st, m, err, errlen);
    /* unknown LVO: the same rule as unknown tags — classified gap, ledger, abort */
    if (g_ledger_n < 16) { g_ledger[g_ledger_n].lvo = lvo; g_ledger[g_ledger_n].tag = 0;
                           g_ledger_n++; }
    snprintf(err, errlen, "capability gap: unknown LVO %d", lvo);
    return MARSH_EGAP;
}

/* ================================ the proof ======================================== */

/* guest memory layout for the fixtures (inside the sandbox, below PROG_ORIGIN) */
#define G_BUF     0x00238000u   /* SpikeRead target buffer                */
#define G_MSG     0x00239000u   /* the guest SpikeMsg                     */
#define G_NAME    0x00239100u   /* "ping" cstr the msg points at          */
#define G_TAGS    0x00239200u   /* the taglist                            */
#define G_TNAME   0x00239300u   /* "amiga" cstr TAG_NAME points at        */
#define G_HOOKSP  0x0023FF00u   /* 68k SP for nested hook runs            */

int main(void)
{
    char err[256] = {0};

    uint8_t *mem = calloc(1, SANDBOX_SIZE);
    CHECK(mem != NULL, "arena");
    j4_sandbox sb;
    j4_sandbox_init(&sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    sb.next_alloc = PROG_ORIGIN;

    marsh_env m; memset(&m, 0, sizeof m);
    m.sb = &sb; m.hook_sp = G_HOOKSP; m.ctx = &m;
    m.eng = j5d_engine_new(); CHECK(m.eng != NULL, "engine");
    j5d_engine_activate(m.eng);

    struct j5d_m68k_state st; memset(&st, 0, sizeof st);

    /* ---- 3. OPAQUE HANDLE first (SpikeOpen gives the handle the others use) ---- */
    memcpy(j4_sandbox_host(&sb, G_NAME), "ping", 5);
    st.a[0] = G_NAME;
    CHECK(spike_dispatch(3, &st, &m, err, sizeof err) == 0, err);
    uint32_t h = st.d[0];
    CHECK(h == 0xBEEF0000u && g_nat.open_count == 1, "SpikeOpen returned a table handle");

    /* a FORGED handle is rejected cleanly */
    st.d[0] = 0xDEAD0001u; st.a[0] = G_BUF; st.d[1] = 4;
    CHECK(spike_dispatch(1, &st, &m, err, sizeof err) == MARSH_EFAULT,
          "forged handle rejected (MARSH_EFAULT)");

    /* ---- 1. BUFFER+LENGTH: SpikeRead fills a guest buffer through the OUT ptr ---- */
    st.d[0] = h; st.a[0] = G_BUF; st.d[1] = 64;
    CHECK(spike_dispatch(1, &st, &m, err, sizeof err) == 0, err);
    CHECK(st.d[0] == 64 && g_nat.read_len == 64, "SpikeRead returned the length");
    { const uint8_t *b = j4_sandbox_host(&sb, G_BUF);
      int ok = 1; for (int i = 0; i < 64; i++) if (b[i] != (uint8_t)(i ^ 0x5A)) ok = 0;
      CHECK(ok, "guest buffer holds the native-written pattern"); }

    /* an out-of-sandbox length is a clean fault, not a host crash */
    st.d[0] = h; st.a[0] = SANDBOX_ORIGIN + SANDBOX_SIZE - 8; st.d[1] = 64;
    CHECK(spike_dispatch(1, &st, &m, err, sizeof err) == MARSH_EFAULT,
          "buffer crossing the sandbox end rejected (MARSH_EFAULT)");

    /* ---- 2. SHADOW STRUCT: PutMsg-class native traversal + OUT copy-back ---- */
    { uint8_t *g = j4_sandbox_host(&sb, G_MSG);
      pbe32(g + 0, G_NAME);                       /* name ptr (guest)            */
      g[4] = 0x12; g[5] = 0x34;                   /* length = 0x1234 BE          */
      pbe32(g + 6, 0xCAFE0001u);                  /* payload (unaligned u32)     */
      pbe32(g + 10, 0); }                         /* reply, to be written back   */
    st.d[0] = h; st.a[0] = G_MSG;
    CHECK(spike_dispatch(2, &st, &m, err, sizeof err) == 0, err);
    CHECK(strcmp(g_nat.putmsg_name, "ping") == 0, "native saw the guest name through the shadow");
    CHECK(g_nat.putmsg_length == 0x1234, "native saw the BE u16 field");
    CHECK(g_nat.putmsg_payload == 0xCAFE0001u, "native saw the unaligned BE u32 field");
    CHECK(gbe32(j4_sandbox_host(&sb, G_MSG) + 10) == 0xCAFE0002u,
          "the OUT field copy-back landed big-endian in guest memory");

    /* ---- 5. TAGLIST: known tags marshal by kind; unknown tag = capability gap ---- */
    memcpy(j4_sandbox_host(&sb, G_TNAME), "amiga", 6);
    { uint8_t *t = j4_sandbox_host(&sb, G_TAGS);
      pbe32(t + 0,  TAG_WIDTH); pbe32(t + 4,  640);
      pbe32(t + 8,  TAG_NAME);  pbe32(t + 12, G_TNAME);
      pbe32(t + 16, 0); pbe32(t + 20, 0); }
    st.a[0] = G_TAGS;
    CHECK(spike_dispatch(5, &st, &m, err, sizeof err) == 0, err);
    CHECK(g_nat.attrs_width == 640 && strcmp(g_nat.attrs_name, "amiga") == 0,
          "taglist marshalled per-kind (scalar + pointer)");
    { uint8_t *t = j4_sandbox_host(&sb, G_TAGS);       /* now poison with an unknown */
      pbe32(t + 16, 0x9999BEEFu); pbe32(t + 20, 7);
      pbe32(t + 24, 0); pbe32(t + 28, 0); }
    int lg = g_ledger_n;
    CHECK(spike_dispatch(5, &st, &m, err, sizeof err) == MARSH_EGAP,
          "unknown tag aborts as a classified capability gap");
    CHECK(g_ledger_n == lg + 1 && g_ledger[lg].tag == 0x9999BEEFu,
          "the gap landed in the ledger with the offending tag");

    /* unknown LVO: same classified-gap rule */
    lg = g_ledger_n;
    CHECK(spike_dispatch(42, &st, &m, err, sizeof err) == MARSH_EGAP, "unknown LVO = gap");
    CHECK(g_ledger_n == lg + 1 && g_ledger[lg].lvo == 42, "LVO gap ledger record");

    /* ---- 4. CALLBACK HOOK, END-TO-END through real 68k code ---- */
    /* the hook: move.l (a1),d0 ; add.l d0,d0 ; rts  (doubles *msg)               */
    /* the caller: lea hook(pc),a0 ; lea msg(pc),a1 ; jsr -24(a6) ; rts           */
    /* laid out by hand as one CODE hunk; msg holds BE 21 -> expect D0 = 42       */
    {
        /* byte 0: 41FA 000C  lea 12(pc),a0   ; ext @2  -> 2+12  = hook @ byte 14 */
        /* byte 4: 43FA 000E  lea 14(pc),a1   ; ext @6  -> 6+14  = msg  @ byte 20 */
        /* byte 8: 4EAE FFE8  jsr -24(a6)     ; LVO 4                             */
        /* byte12: 4E75       rts                                                 */
        /* byte14: 2011 D080 4E75             ; hook: move.l (a1),d0 ; add ; rts  */
        /* byte20: 0000 0015                  ; msg: BE 21                        */
        static const uint32_t hookprog[] = {
            0x41FA000Cu, 0x43FA000Eu, 0x4EAEFFE8u, 0x4E752011u,
            0xD0804E75u, 0x00000015u,
        };
        uint8_t hb[256];
        uint32_t w[32]; unsigned n = 0;
        w[n++] = 0x3F3; w[n++] = 0; w[n++] = 1; w[n++] = 0; w[n++] = 0;
        w[n++] = 6; w[n++] = 0x3E9; w[n++] = 6;
        for (int i = 0; i < 6; i++) w[n++] = hookprog[i];
        w[n++] = 0x3F2;
        for (unsigned i = 0; i < n; i++) pbe32(hb + i*4, w[i]);

        j4_seglist seg;
        CHECK(j4_load_hunks(&sb, hb, n * 4, 0, &seg, err, sizeof err) == 0, err);
        struct j5d_m68k_state pst; memset(&pst, 0, sizeof pst);
        uint32_t d0 = 0;
        j5d_sandbox j5sb = { sb.host_mem, sb.sandbox_origin, sb.size };
        int rc = j5d_run(&j5sb, seg.entry, LIBBASE, &pst, &d0,
                         spike_dispatch, &m, err, sizeof err);
        CHECK(rc == 0, err);
        CHECK(d0 == 42, "hook end-to-end: 68k caller -> LVO bridge -> descriptor "
                        "marshaller -> native -> NESTED 68k hook -> D0=42 back out");
    }

    j5d_engine_free(m.eng);
    free(mem);

    printf("[T0P4] PASS: the annotation vocabulary survives all five representative "
           "cases — buffer+length with clean bounds faults, a natively-traversed "
           "shadow struct with big-endian copy-back, validated opaque handles, a "
           "native->68k callback hook driven end-to-end through the LVO bridge with "
           "a nested engine entry, and per-kind taglists where unknown tags and "
           "unknown LVOs abort as ledger-recorded capability gaps.\n");
    return 0;
}
