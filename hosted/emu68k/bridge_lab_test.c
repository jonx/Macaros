/* Focused safety test for Bridge Lab's bounded JSONL recorder. */
#include "bridge_lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* bridge_lab.c deliberately uses the host lookup wrapper in production. */
const char *emu68k_host_getenv(const char *name)
{
    return getenv(name);
}

int main(void)
{
    char path[] = "/tmp/bridge-lab-cap.XXXXXX";
    char data[8192];
    struct stat st;
    FILE *in;
    int fd = mkstemp(path);
    size_t n;

    if (fd < 0) return 1;
    close(fd);
    setenv("EMU68K_BRIDGE_TRACE", path, 1);
    setenv("EMU68K_BRIDGE_TRACE_LEVEL", "debug", 1);
    setenv("EMU68K_BRIDGE_TRACE_MAX_BYTES", "1024", 1);

    bl_open("cap-test");
    for (int i = 0; i < 200; i++)
        bl_event(BL_RUNTIME, 0, 0x210000u, 0x250000u,
                 "test.detail",
                 "\"iteration\":%d,\"payload\":\"0123456789abcdef"
                 "0123456789abcdef0123456789abcdef\"", i);
    bl_close("ok");

    if (stat(path, &st) != 0 || st.st_size > 1024 + 65536) goto fail;
    in = fopen(path, "r");
    if (!in) goto fail;
    n = fread(data, 1, sizeof data - 1, in);
    fclose(in);
    data[n] = '\0';
    if (!strstr(data, "\"event\":\"trace.truncated\"") ||
        !strstr(data, "\"event\":\"run.end\"") ||
        !strstr(data, "\"result\":\"ok\""))
        goto fail;
    unlink(path);
    puts("[BRIDGE-CAP] PASS: runtime trace capped; truncation and run-end "
         "evidence retained.");
    return 0;

fail:
    unlink(path);
    fputs("[BRIDGE-CAP] FAIL: cap or retained summary contract broken.\n",
          stderr);
    return 1;
}
