// AROS AArch64 bring-up — M8: a minimal line shell over the UART.
//
// The harness injects keystrokes into the serial socket (the "drive" half of the
// observation channel); we echo and dispatch. Foreshadows a real AROS CLI.

#include "kern.h"

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void shell_run(void) {
    char line[64];
    kprintf("[M8a] shell ready (commands: ping, ticks, quit)\n");
    for (;;) {
        kprintf("shell> ");
        int n = 0, ch;
        while ((ch = uart_getc()) != '\r' && ch != '\n') {
            if (ch == 0x7f || ch == 0x08) {            // backspace
                if (n) { n--; kprintf("\b \b"); }
                continue;
            }
            if (n < (int)sizeof(line) - 1) { line[n++] = (char)ch; uart_putc((char)ch); }
        }
        uart_putc('\n');
        line[n] = 0;
        if (streq(line, "ping"))       kprintf("pong\n");
        else if (streq(line, "ticks")) kprintf("ticks=%lu\n", timer_ticks);
        else if (streq(line, "quit"))  { kprintf("[M8] shell ok (got quit)\n"); return; }
        else if (n)                    kprintf("unknown: %s\n", line);
    }
}
