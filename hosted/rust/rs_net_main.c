/* rs_net_main.c -- RS4 harness: C owns AROS startup, calls the Rust bsdsocket
 * round-trip (aros_rust_net_test prints its own PASS/FAIL through std). The exit
 * code reflects the result for the unattended loop. */
#include <proto/dos.h>

#define AROS_RS7_MAGIC 0x52533700u   /* "RS7 " */

extern unsigned int aros_rust_net_test(void);

/* std's args pal (sys/args/aros.rs) is now part of std and gets pulled into the
 * link, so every std harness must supply these (see rs3_main.c). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    return aros_rust_net_test() == AROS_RS7_MAGIC ? 0 : 20;
}
