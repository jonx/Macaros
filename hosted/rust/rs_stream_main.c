/* rs_stream_main.c -- STREAM harness: large streamed read byte-exact verify
 * (blocking + non-blocking) to isolate the LSP-over-socket recv corruption.
 * aros_rust_stream_test prints its own PASS/FAIL through std. */
#include <proto/dos.h>

#define AROS_STREAM_MAGIC 0x53545200u   /* "STR " */

extern unsigned int aros_rust_stream_test(void);

int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    return aros_rust_stream_test() == AROS_STREAM_MAGIC ? 0 : 20;
}
