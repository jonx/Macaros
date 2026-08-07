/* bridge_lab.c - the runtime event recorder. See bridge_lab.h for why. */

#include "bridge_lab.h"
#include "emu68k_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define BL_MAX_IDS 512
#define BL_ID_TEXT 32
#define BL_DEFAULT_MAX_BYTES (32ULL * 1024ULL * 1024ULL)
#define BL_SUMMARY_RESERVE    (64ULL * 1024ULL)

static FILE     *g_out;
static int       g_level = -1;
static unsigned long g_seq;
static int       g_run;      /* which run in this trace file we are in */
static unsigned long long g_max_bytes;
static int       g_limit_ready;
static int       g_truncated;

/* One identity table per namespace, numbered in first-seen order. */
static struct {
    char     kind[16];
    uint32_t addr;
    char     text[BL_ID_TEXT];
} g_ids[BL_MAX_IDS];
static int g_nids;
static int g_counts[16];      /* next number per kind, by kind-table index    */
static char g_kinds[16][16];
static int g_nkinds;

static void bl_read_limit(void)
{
    const char *value;
    char *end = NULL;
    unsigned long long parsed;

    if (g_limit_ready) return;
    g_limit_ready = 1;
    g_max_bytes = BL_DEFAULT_MAX_BYTES;
    value = emu68k_host_getenv("EMU68K_BRIDGE_TRACE_MAX_BYTES");
    if (!value || !*value) return;
    parsed = strtoull(value, &end, 10);
    if (end && *end == '\0')
        g_max_bytes = parsed; /* zero explicitly means unlimited */
}

/* Runtime detail stops at the soft limit. Summary/failure records retain a
 * small reserve so a capped trace still says that it was capped and how the
 * run ended. Nothing in this path changes guest execution. */
static int bl_may_write(int level)
{
    long pos;

    bl_read_limit();
    if (!g_max_bytes) return 1;
    pos = ftell(g_out);
    if (pos < 0 || (unsigned long long)pos < g_max_bytes) return 1;

    if (!g_truncated &&
        (unsigned long long)pos < g_max_bytes + BL_SUMMARY_RESERVE) {
        g_truncated = 1;
        fprintf(g_out,
                "{\"schema\":1,\"seq\":%lu,\"run\":%d,"
                "\"event\":\"trace.truncated\","
                "\"limit_bytes\":%llu,\"bytes_at_limit\":%llu}\n",
                ++g_seq, g_run, g_max_bytes, (unsigned long long)pos);
        /* The truncation marker is operational evidence: make it visible to a
         * live reporter even when the long-running application never closes
         * this stream during the diagnostic session. */
        fflush(g_out);
        pos = ftell(g_out);
    }
    if (level > BL_SUMMARY) return 0;
    return pos < 0 ||
           (unsigned long long)pos < g_max_bytes + BL_SUMMARY_RESERVE;
}

int bl_level(void)
{
    if (g_level < 0) {
        const char *lv = emu68k_host_getenv("EMU68K_BRIDGE_TRACE_LEVEL");
        const char *to = emu68k_host_getenv("EMU68K_BRIDGE_TRACE");
        if (!to || !*to) { g_level = BL_OFF; return g_level; }
        g_level = BL_RUNTIME;                       /* the useful default     */
        if (lv) {
            if (!strcmp(lv, "off"))          g_level = BL_OFF;
            else if (!strcmp(lv, "summary")) g_level = BL_SUMMARY;
            else if (!strcmp(lv, "runtime")) g_level = BL_RUNTIME;
            else if (!strcmp(lv, "calls"))   g_level = BL_CALLS;
            else if (!strcmp(lv, "debug"))   g_level = BL_DEBUG;
        }
    }
    return g_level;
}

const char *bl_id(const char *kind, uint32_t addr)
{
    int i, k = -1;
    for (i = 0; i < g_nids; i++)
        if (g_ids[i].addr == addr && !strcmp(g_ids[i].kind, kind))
            return g_ids[i].text;
    for (i = 0; i < g_nkinds; i++)
        if (!strcmp(g_kinds[i], kind)) { k = i; break; }
    if (k < 0 && g_nkinds < 16) {
        k = g_nkinds++;
        snprintf(g_kinds[k], sizeof g_kinds[k], "%s", kind);
        g_counts[k] = 0;
    }
    if (g_nids >= BL_MAX_IDS || k < 0) return "?";
    i = g_nids++;
    snprintf(g_ids[i].kind, sizeof g_ids[i].kind, "%s", kind);
    g_ids[i].addr = addr;
    /* NAMESPACED BY RUN. A sweep appends several programs to one trace, and a
     * bump allocator hands out the same guest addresses to each of them - so
     * without this, one program's port:1 and another's are the same string for
     * different objects, and any check that groups by identity silently merges
     * two programs' evidence. */
    snprintf(g_ids[i].text, sizeof g_ids[i].text, "r%d/%s:%d",
             g_run, kind, ++g_counts[k]);
    return g_ids[i].text;
}

void bl_open(const char *program)
{
    const char *to;
    if (bl_level() == BL_OFF) return;
    to = emu68k_host_getenv("EMU68K_BRIDGE_TRACE");
    /* APPEND. A sweep runs several programs through one loaded runtime, and
     * truncating per program left only the last one's evidence - which looked
     * exactly like a contract that was never exercised. The sequence number is
     * process-global, so slicing by seq still works across runs, and each run
     * is delimited by its own run.start/run.end. */
    g_out = fopen(to, "a");
    if (!g_out) { g_level = BL_OFF; return; }
    g_run++;
    g_truncated = 0;
    g_nids = 0;                  /* identities are per run, see bl_id */
    g_nkinds = 0;
    /* Unconditional, so "no file" and "no events" are different answers.
     *
     * The TREE this was measured on travels with the result. An hour was spent
     * reasoning about commits while the checkout sat on a branch that did not
     * contain them - same file path, different content - and every conclusion
     * drawn in that hour was void. A result that does not say which tree
     * produced it cannot be trusted later, and neither can a comparison
     * between two of them. */
    {
        const char *tree = emu68k_host_getenv("EMU68K_BRIDGE_TREE");
        bl_event(BL_SUMMARY, -1, 0, 0, "run.start",
                 "\"program\":\"%s\",\"tree\":\"%s\"",
                 program ? program : "", tree ? tree : "unrecorded");
    }
}

void bl_close(const char *result)
{
    if (!g_out) return;
    bl_event(BL_SUMMARY, -1, 0, 0, "run.end", "\"result\":\"%s\"",
             result ? result : "unknown");
    fclose(g_out);
    g_out = NULL;
    /* Do NOT latch the level off: the next program in this sweep gets its own
     * run.start appended to the same file. */
}

void bl_event(int level, int context, uint32_t task, uint32_t pc,
              const char *event, const char *fields, ...)
{
    if (!g_out || bl_level() < level) return;
    if (!bl_may_write(level)) return;
    fprintf(g_out, "{\"schema\":1,\"seq\":%lu,\"run\":%d", ++g_seq, g_run);
    if (context >= 0) fprintf(g_out, ",\"context\":%d", context);
    if (task)         fprintf(g_out, ",\"task\":\"%s\"", bl_id("task", task));
    if (pc)           fprintf(g_out, ",\"pc\":\"0x%08x\"", pc);
    fprintf(g_out, ",\"event\":\"%s\"", event);
    if (fields && *fields) {
        va_list ap;
        fputc(',', g_out);
        va_start(ap, fields);
        vfprintf(g_out, fields, ap);
        va_end(ap);
    }
    fputs("}\n", g_out);
}
