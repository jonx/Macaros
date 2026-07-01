/* rs_moonstone_main.c -- AROS harness for the Moonstone render probe. C owns startup,
 * calls the Rust entry (aros_moonstone_render prints its own progress via std), and
 * maps the result to an exit code for the unattended loop. */
#include <proto/dos.h>

#define MOONSTONE_MAGIC 0x4D4F4F4Eu   /* "MOON" */

extern unsigned int aros_moonstone_render(void);

/* std's args pal reads these (see rs3_main.c). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    aros_argc = argc;
    aros_argv = argv;
    return aros_moonstone_render() == MOONSTONE_MAGIC ? 0 : 20;
}
