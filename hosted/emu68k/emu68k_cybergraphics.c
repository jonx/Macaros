/* Handwritten cybergraphics.library guest-memory semantics.
 * Most cybergraphics crossings are generated; exceptional mirrors live here. */
#include "emu68k_internal.h"

int emu68k_cybergraphics_call(struct emu68k_run *r, j4_sandbox *sb, int lvo,
                              struct j5d_m68k_state *st, char *e, unsigned el)
{
    (void)r; (void)sb; (void)lvo; (void)st; (void)e; (void)el;
    return 1;
}
