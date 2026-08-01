/* scan68k_main.c — the `scan68k` diagnosis CLI (OURS, AROS-licensed).
 *
 *   scan68k <program.exe>      how would this 68k program run here, and why
 *   scan68k -q <program.exe>   one line: "<ROUTE> <confidence> <evidence-count>"
 *
 * Prints the route the router would pick and the evidence behind it. The static
 * scan is a prediction, not a verdict: it cannot see addresses computed at run
 * time, so a "JIT" answer can still turn into a hardware fault once running -
 * which the engine's runtime guard catches exactly. Running it is always the
 * authority; this is the inspectable prediction (and the same core the in-OS
 * explanation uses). */

#include "scan68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kind_name(scan68k_evkind k)
{
    switch (k) {
    case SCAN68K_EV_PRIVILEGED: return "privileged";
    case SCAN68K_EV_CUSTOM:     return "custom-chip";
    case SCAN68K_EV_CIA:        return "CIA";
    case SCAN68K_EV_VECTOR:     return "vector-page";
    }
    return "?";
}

int main(int argc, char **argv)
{
    int quiet = 0, argi = 1;
    if (argc > 1 && !strcmp(argv[1], "-q")) { quiet = 1; argi = 2; }
    if (argi >= argc) {
        fprintf(stderr, "usage: scan68k [-q] <program.exe>\n");
        return 2;
    }

    FILE *f = fopen(argv[argi], "rb");
    if (!f) { fprintf(stderr, "scan68k: cannot open %s\n", argv[argi]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "scan68k: cannot read %s\n", argv[argi]); return 1;
    }
    fclose(f);

    scan68k_report r;
    char err[128] = {0};
    if (scan68k_image(buf, (unsigned long)sz, &r, err, sizeof err)) {
        fprintf(stderr, "scan68k: %s: %s\n", argv[argi], err);
        return 1;
    }

    if (quiet) {
        printf("%s %d %d\n", scan68k_route(&r), (int)r.confidence, r.n_evidence);
        return 0;
    }

    printf("%s\n", argv[argi]);
    printf("  route      : %s\n", scan68k_route(&r));
    printf("  verdict    : %s\n", scan68k_confidence_text(r.confidence));
    printf("  scanned    : %d code hunk%s, %lu bytes\n",
           r.n_code_hunks, r.n_code_hunks == 1 ? "" : "s", r.code_bytes);
    if (r.n_evidence == 0) {
        printf("  evidence   : none\n");
    } else {
        printf("  evidence   : %d finding%s%s\n", r.n_evidence,
               r.n_evidence == 1 ? "" : "s", r.truncated ? " (more not shown)" : "");
        for (int i = 0; i < r.n_evidence; i++) {
            const scan68k_evidence *e = &r.evidence[i];
            printf("    hunk %d +0x%04lx  %-11s %s%s\n",
                   e->hunk, e->offset, kind_name(e->kind), e->what,
                   e->in_context ? "" : "  [weak]");
        }
    }
    printf("\n");
    if (r.confidence == SCAN68K_BANGER)
        printf("  This program drives the Amiga hardware, which translation cannot\n"
               "  serve. It needs a full machine emulator.\n");
    else if (r.confidence == SCAN68K_SUSPECT)
        printf("  Hardware-shaped bytes appear, but never as a hardware instruction,\n"
               "  so this is most likely data. It runs under translation, with the\n"
               "  runtime guard watching.\n");
    else
        printf("  Nothing here needs the Amiga hardware; it should run under\n"
               "  translation. Addresses computed at run time cannot be seen from\n"
               "  the outside, so the runtime guard remains the authority.\n");
    return 0;
}
