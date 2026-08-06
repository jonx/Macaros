/* Minimal external command used to provision Regina trip.rexx's command-path
 * probe. It deliberately has ordinary AROS startup code: the test therefore
 * covers LoadSeg + RunCommand, not a hand-crafted return stub. */
int main(void)
{
    return 0;
}
