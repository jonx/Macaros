/* [T3e] Guest-side 68k library lifecycle proof.
 *
 * Proves the two Resident forms are distinct and that a registered GUEST_68K
 * base executes its vector code rather than entering the native LVO bridge:
 *   - testlib.s: direct rt_Init code returns a hand-built guest base;
 *   - autoinitlib.s: two named residents cover relative/absolute MakeFunctions
 *     tables and InitStruct through the reusable guestlib68k construction core;
 *   - callers use the real `jsr d16(a6)` form, so an untyped base registry
 *     would invoke fail_bridge and fail the test.
 */

#include "guestlib68k.h"
#include "j4_hunk.h"
#include "j5d_jit68k.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORIGIN    0x00200000u
#define ARENA_LEN 0x00400000u
#define EXEC_BASE 0x00210000u
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "[T3E] FAIL: %s\n", (m)); return 1; } } while (0)

typedef struct loaded_lib {
    j4_seglist seg;
    gl68_resident resident;
} loaded_lib;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return NULL;
    }
    fclose(f); *len = (size_t)n; return p;
}

static int load_resident(j4_sandbox *sb, const char *path, const char *name,
                         loaded_lib *out, char *err, unsigned errlen)
{
    size_t len = 0;
    uint8_t *file = slurp(path, &len);
    if (!file) { snprintf(err, errlen, "cannot read %s", path); return 1; }
    if (j4_load_hunks(sb, file, len, 0, &out->seg, err, errlen)) {
        free(file); return 1;
    }
    free(file);

    return gl68_find_resident(sb, &out->seg, name, &out->resident, err, errlen);
}

static uint32_t guest_alloc(j4_sandbox *sb, uint32_t bytes)
{
    uint32_t at = (sb->next_alloc + 15u) & ~15u;
    uint32_t n = (bytes + 15u) & ~15u;
    if ((uint64_t)at + n > (uint64_t)sb->sandbox_origin + sb->size) return 0;
    sb->next_alloc = at + n;
    memset(j4_sandbox_host(sb, at), 0, n);
    return at;
}

static int bridge_hits;
static int fail_bridge(int lvo, struct j5d_m68k_state *st, void *user,
                       char *err, unsigned errlen)
{
    (void)st; (void)user;
    bridge_hits++;
    snprintf(err, errlen, "guest library LVO %d incorrectly entered native bridge", lvo);
    return 1;
}

static int run_pc(j4_sandbox *sb, uint32_t pc, uint32_t a6,
                  struct j5d_m68k_state *st, uint32_t *d0, char *err, unsigned errlen)
{
    st->a[7] = (sb->sandbox_origin + sb->size - 16u) & ~15u;
    return j5d_run((j5d_sandbox *)sb, pc, a6, st, d0,
                   fail_bridge, NULL, err, errlen);
}

/* Emit a tiny real caller: moveq #x,d0; moveq #y,d1; jsr disp(a6); rts. */
static uint32_t make_caller(j4_sandbox *sb, uint8_t x, uint8_t y, int16_t disp)
{
    uint32_t pc = guest_alloc(sb, 10);
    uint8_t *p = j4_sandbox_host(sb, pc);
    put16(p + 0, (uint16_t)(0x7000u | x));
    put16(p + 2, (uint16_t)(0x7200u | y));
    put16(p + 4, 0x4eaeu);                 /* jsr (d16,a6) */
    put16(p + 6, (uint16_t)disp);
    put16(p + 8, 0x4e75u);                 /* rts          */
    return pc;
}

static int call_vector(j4_sandbox *sb, uint32_t base, int16_t disp,
                       uint8_t x, uint8_t y, uint32_t *d0,
                       char *err, unsigned errlen)
{
    struct j5d_m68k_state st;
    uint32_t caller = make_caller(sb, x, y, disp);
    if (!caller) { snprintf(err, errlen, "no room for vector caller"); return 1; }
    memset(&st, 0, sizeof st);
    return run_pc(sb, caller, base, &st, d0, err, errlen);
}

static int prove_direct(j4_sandbox *sb, loaded_lib *l, char *err, unsigned errlen)
{
    struct j5d_m68k_state st;
    gl68_init init;
    uint32_t base = 0, got = 0;

    CHECK((l->resident.flags & GL68_RTF_AUTOINIT) == 0,
          "test.library must be a direct-init resident");
    CHECK(gl68_prepare_init(sb, &l->seg, &l->resident, &init, err, errlen) == 0, err);
    CHECK(init.base == 0 && init.init_pc == l->resident.init,
          "direct resident was not prepared as executable rt_Init");
    memset(&st, 0, sizeof st);
    st.a[0] = init.seglist;                /* seglist stand-in for this proof */
    CHECK(run_pc(sb, init.init_pc, EXEC_BASE, &st, &base, err, errlen) == 0, err);
    CHECK(base != 0, "direct init did not return its in-guest library base");

    j5d_register_guest_libbase(base);
    CHECK(call_vector(sb, base, -30, 19, 23, &got, err, errlen) == 0, err);
    CHECK(got == 42, "direct guest TestAdd did not return 42");
    CHECK(call_vector(sb, base, -36, 0, 0, &got, err, errlen) == 0, err);
    CHECK(got == 0x5afec0deu, "direct guest TestMagic returned the wrong marker");
    return 0;
}

static int prove_autoinit(j4_sandbox *sb, loaded_lib *l, char *err, unsigned errlen)
{
    gl68_init init;
    gl68_init abs_init;
    gl68_resident abs_resident;
    uint32_t base, got = 0;
    struct j5d_m68k_state st;

    CHECK((l->resident.flags & GL68_RTF_AUTOINIT) != 0,
          "autoinit.library must carry RTF_AUTOINIT");
    CHECK(gl68_prepare_init(sb, &l->seg, &l->resident, &init, err, errlen) == 0, err);
    CHECK(init.pos_size == 64 && init.vectors == 6 && init.base != 0,
          "AutoInit construction did not produce a 64-byte base with six vectors");
    base = init.base;
    CHECK(be32(j4_sandbox_host(sb, base + 10u)) == l->resident.name_ptr,
          "InitResident did not copy the resident name into struct Library");
    CHECK(j4_sandbox_host(sb, base)[20] == 0 &&
          j4_sandbox_host(sb, base)[21] == l->resident.version,
          "InitResident did not copy the resident version");
    CHECK(be32(j4_sandbox_host(sb, base + 52u)) == 0x1a1757c7u,
          "MakeLibrary did not apply the InitStruct stream");

    memset(&st, 0, sizeof st);
    st.d[0] = base; st.a[0] = init.seglist;
    CHECK(run_pc(sb, init.init_pc, EXEC_BASE, &st, &got, err, errlen) == 0, err);
    CHECK(got == base, "AutoInit final callback did not preserve its library base");
    CHECK(be32(j4_sandbox_host(sb, base + 56u)) == 0xa17e1a17u,
          "AutoInit final callback did not initialize the positive base");

    j5d_register_guest_libbase(base);
    CHECK(call_vector(sb, base, -6, 0, 0, &got, err, errlen) == 0, err);
    CHECK(got == base, "AutoInit Open vector did not return its guest base");
    CHECK(call_vector(sb, base, -30, 20, 22, &got, err, errlen) == 0, err);
    CHECK(got == 42, "AutoInit public TestAdd did not return 42");
    CHECK(call_vector(sb, base, -36, 0, 0, &got, err, errlen) == 0, err);
    CHECK(got == 0xa170c0deu, "AutoInit public TestMagic returned the wrong marker");

    CHECK(gl68_find_resident(sb, &l->seg, "autoinitabs.library", &abs_resident,
                             err, errlen) == 0,
          "second named resident was not selected from the same hunk");
    CHECK(gl68_prepare_init(sb, &l->seg, &abs_resident, &abs_init,
                            err, errlen) == 0, err);
    CHECK(abs_init.base != 0 && abs_init.vectors == 6,
          "absolute MakeFunctions table did not construct six vectors");
    j5d_register_guest_libbase(abs_init.base);
    CHECK(call_vector(sb, abs_init.base, -30, 21, 21, &got,
                      err, errlen) == 0, err);
    CHECK(got == 42, "absolute-table guest TestAdd did not return 42");
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t *arena;
    j4_sandbox sb;
    loaded_lib direct, autoinit;
    j5d_engine *engine;
    char err[256] = {0};

    if (argc != 3) {
        fprintf(stderr, "usage: %s test.library autoinit.library\n", argv[0]);
        return 2;
    }
    arena = calloc(1, ARENA_LEN);
    CHECK(arena != NULL, "cannot allocate guest arena");
    CHECK(j4_sandbox_init(&sb, arena, ORIGIN, ARENA_LEN) == 0,
          "cannot initialize guest arena");
    memset(&direct, 0, sizeof direct); memset(&autoinit, 0, sizeof autoinit);
    CHECK(load_resident(&sb, argv[1], "test.library", &direct, err, sizeof err) == 0, err);
    CHECK(load_resident(&sb, argv[2], "autoinit.library", &autoinit, err, sizeof err) == 0, err);

    /* AROS's ROM scanner accepts a backward rt_EndSkip and simply continues
     * scanning from the next word.  Its own aros.library has this layout after
     * ELF-to-HUNK conversion, so keep that compatibility under regression. */
    {
        gl68_resident backward;
        uint8_t *field = j4_sandbox_host(&sb, direct.resident.tag + 6u);
        uint32_t saved = be32(field);
        CHECK(direct.seg.hunk_base[0] < direct.resident.tag,
              "fixture cannot exercise a backward rt_EndSkip");
        put32(field, direct.seg.hunk_base[0]);
        CHECK(gl68_find_resident(&sb, &direct.seg, "test.library", &backward,
                                 err, sizeof err) == 0,
              "AROS-compatible backward rt_EndSkip was rejected");
        put32(field, saved);
    }

    engine = j5d_engine_new();
    CHECK(engine != NULL, "cannot allocate JIT engine instance");
    j5d_engine_activate(engine);
    j5d_clear_libbases();
    bridge_hits = 0;

    CHECK(prove_direct(&sb, &direct, err, sizeof err) == 0, err);
    CHECK(prove_autoinit(&sb, &autoinit, err, sizeof err) == 0, err);
    CHECK(bridge_hits == 0, "a guest library call entered the native bridge");

    j5d_engine_activate(NULL);
    j5d_engine_free(engine);
    free(arena);
    printf("[T3E] PASS: direct rt_Init and named RTF_AUTOINIT residents distinguished; "
           "relative + absolute MakeFunctions tables and InitStruct constructed guest "
           "bases; jsr d16(a6) executed guest vectors with zero native-bridge hits\n");
    return 0;
}
