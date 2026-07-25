/* rs_stack_main.c -- STACK harness: thread stacks are the size std asks for. */
#include <proto/dos.h>
#define AROS_STACK_MAGIC 0x5354434bu   /* "STCK" */
extern unsigned int aros_rust_stack_test(void);
int aros_argc = 0;
char **aros_argv = 0;
int main(int argc, char **argv)
{
    aros_argc = argc; aros_argv = argv;
    return aros_rust_stack_test() == AROS_STACK_MAGIC ? 0 : 20;
}
