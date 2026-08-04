/* Target-side DOS name-path probe.  It avoids CLI argument parsing so the
 * fixture can distinguish a handler limitation from a command limitation. */
#include <dos/dos.h>
#include <proto/dos.h>
#include <string.h>

int main(void)
{
    char path[sizeof("EXFAT4:") + 255];
    BPTR file;
    char byte;

    memcpy(path, "EXFAT4:", sizeof("EXFAT4:") - 1);
    memset(path + sizeof("EXFAT4:") - 1, 'L', 251);
    memcpy(path + sizeof("EXFAT4:") - 1 + 251, ".bin", 5);
    file = Open(path, MODE_OLDFILE);
    if (file == BNULL)
    {
        Printf("[EXFATNAME] FAIL Open 255-unit name (%ld)\n", IoErr());
        return 20;
    }
    if (Read(file, &byte, 1) != 1 || byte != 'l')
    {
        Close(file);
        Printf("[EXFATNAME] FAIL Read 255-unit name (%ld)\n", IoErr());
        return 20;
    }
    Close(file);
    Printf("[EXFATNAME] PASS 255-unit name opens through DOS\n");
    return 0;
}
