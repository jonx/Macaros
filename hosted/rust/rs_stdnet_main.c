/* rs_stdnet_main.c -- RSN harness: C owns AROS startup, calls the Rust std::net
 * round-trip (aros_rust_stdnet_test prints its own PASS/FAIL through std). Unlike
 * the RS4 RustNet, this exercises real std::net::TcpStream (the net pal), not the
 * bsdsocket glue directly. Exit code reflects the result for the unattended loop. */
#include <proto/dos.h>

#define AROS_RSN_MAGIC 0x52534e00u   /* "RSN " */

extern unsigned int aros_rust_stdnet_test(void);

/* std's args pal (sys/args/aros.rs) is now part of std and gets pulled into the
 * link, so every std harness must supply these (see rs3_main.c). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    return aros_rust_stdnet_test() == AROS_RSN_MAGIC ? 0 : 20;
}
