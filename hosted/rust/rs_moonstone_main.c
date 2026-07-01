/* rs_moonstone_main.c -- AROS harness for the Moonstone port.
 *   Moonstone           -> live: opens a window, blits the game each frame (aros_moonstone_play)
 *   Moonstone render    -> headless: renders one frame to a PNG (aros_moonstone_render)
 * The Rust side prints its own progress via std. */
#include <proto/dos.h>

#define MOONSTONE_MAGIC 0x4D4F4F4Eu   /* "MOON" */

extern unsigned int aros_moonstone_render(void);
extern unsigned int aros_moonstone_play(void);
extern unsigned int aros_moonstone_game(void);

/* std's args pal reads these (see rs3_main.c). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    unsigned int rc;
    aros_argc = argc;
    aros_argv = argv;

    /*   Moonstone          -> the game (scene stack, live)
     *   Moonstone render   -> headless PNG of one frame
     *   Moonstone demo     -> the shim demo (background + movable cursor) */
    if (argc > 1 && argv[1][0] == 'r')
        rc = aros_moonstone_render();
    else if (argc > 1 && argv[1][0] == 'd')
        rc = aros_moonstone_play();
    else
        rc = aros_moonstone_game();

    return rc == MOONSTONE_MAGIC ? 0 : 20;
}
