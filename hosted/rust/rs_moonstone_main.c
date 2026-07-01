/* rs_moonstone_main.c -- AROS harness for the Moonstone port.
 *   Moonstone           -> live: opens a window, blits the game each frame (aros_moonstone_play)
 *   Moonstone render    -> headless: renders one frame to a PNG (aros_moonstone_render)
 * The Rust side prints its own progress via std. */
#include <proto/dos.h>

#define MOONSTONE_MAGIC 0x4D4F4F4Eu   /* "MOON" */

extern unsigned int aros_moonstone_render(void);
extern unsigned int aros_moonstone_play(void);
extern unsigned int aros_moonstone_game(void);
extern unsigned int aros_moonstone_game_skip(void);

/* std's args pal reads these (see rs3_main.c). */
int aros_argc = 0;
char **aros_argv = 0;

int main(int argc, char **argv)
{
    unsigned int rc;
    char c = (argc > 1 && argv[1][0]) ? argv[1][0] : 0;
    aros_argc = argc;
    aros_argv = argv;

    /*   Moonstone               -> the game (scene stack, live, with intro)
     *   Moonstone skip          -> the game, starting at the menu (skip intro)
     *   Moonstone --skip-intro  -> same (any '-' flag = skip-intro)
     *   Moonstone render        -> headless PNG of one frame
     *   Moonstone demo          -> the shim demo (background + movable cursor) */
    if (c == 'r')
        rc = aros_moonstone_render();
    else if (c == 'd')
        rc = aros_moonstone_play();
    else if (c == 's' || c == '-')
        rc = aros_moonstone_game_skip();
    else
        rc = aros_moonstone_game();

    return rc == MOONSTONE_MAGIC ? 0 : 20;
}
