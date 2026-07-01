/* rs3_main.c -- RS3 harness: C owns AROS startup, calls a Rust function that uses
 * the real `std` (println! routed through the aros stdio pal -> posixc write ->
 * dos Output()). Proves std works on booted AROS, not just no_std.
 *
 * The Rust fn prints "hello from rust std on AROS" itself; this harness then
 * prints one PASS/FAIL line for the unattended loop.
 */
#include <proto/dos.h>     /* PutStr -- no <stdio.h> */

#define AROS_RS3_MAGIC 0x52533320u   /* "RS3 " */

extern unsigned int aros_rust_std_hello(void);

/* Read by sys/args/aros.rs so std::env::args() works (C owns main, not lang_start). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    unsigned int rs3;
    aros_argc = argc;
    aros_argv = argv;
    rs3 = aros_rust_std_hello();
    if (rs3 == AROS_RS3_MAGIC) {
        PutStr("[RS3] rust std PASS (println! via posixc -> dos)\n");
        PutStr("RUST-AROS: STD PASS\n");
        return 0;
    }
    PutStr("[RS3] rust std FAIL\n");
    return 20;
}
