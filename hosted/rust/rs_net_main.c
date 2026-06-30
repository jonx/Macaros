/* rs_net_main.c -- RS4 harness: C owns AROS startup, calls the Rust bsdsocket
 * round-trip (aros_rust_net_test prints its own PASS/FAIL through std). The exit
 * code reflects the result for the unattended loop. */
#include <proto/dos.h>

#define AROS_RS7_MAGIC 0x52533700u   /* "RS7 " */

extern unsigned int aros_rust_net_test(void);

int main(void)
{
    return aros_rust_net_test() == AROS_RS7_MAGIC ? 0 : 20;
}
