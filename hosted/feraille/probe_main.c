/* probe_main.c -- C harness for the Feraille-on-AROS stage-1 probe.
 * Calls the two Rust probe entry points (pure domain logic, then SQLite via
 * feraille-meta) and prints greppable PASS/FAIL markers. posixc write(1)
 * honors shell redirects, so printf is the right output path here. */
#include <stdio.h>

extern unsigned int feraille_core_probe(void);
extern unsigned int feraille_meta_probe(void);

/* The std args pal reads argc/argv from these globals (same contract as
 * hosted/rust/rs3_main.c). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    unsigned int core = feraille_core_probe();
    unsigned int meta = feraille_meta_probe();

    printf("[FA1] core probe digest=0x%08x %s\n", core, core ? "ok" : "FAIL");
    printf("[FA2] meta probe (sqlite) %s\n", meta == 1 ? "ok" : "FAIL");
    if (core && meta == 1) {
        printf("FERAILLE-AROS: CORE PASS\n");
        return 0;
    }
    printf("FERAILLE-AROS: CORE FAIL\n");
    return 20;
}
