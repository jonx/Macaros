/* dhry_glue.c — freestanding 68k glue for the Dhrystone 2.1 bench build (OURS,
 * AROS-licensed; the benchmark sources themselves are fetched + patched by
 * build-dhry.sh and are NOT part of this repo).
 *
 * Provides everything the patched dhry_1/dhry_2 need beyond the CPU:
 *   - dhry_printf: a minimal %d/%c/%s/%% printf over the crt0 putch shim
 *     (exec PutChar, LVO -30 — the same single OS service the whole apps68k
 *     corpus uses), with manual stack varargs (vbcc m68k passes all arguments
 *     on the stack, each slot 32-bit);
 *   - strcpy / strcmp for dhry_2's string block.
 * Deterministic on purpose: fixed DHRY_RUNS, no timing, so the whole output
 * stream is byte-exact comparable across engines and hosts. */

extern void putch(int c);                   /* crt0.s _putch -> PutChar(a6) */

static void put_str(const char *s) { while (*s) putch(*s++); }

static void put_int(long v)
{
    char b[12]; int n = 0;
    unsigned long u = (v < 0) ? (putch('-'), (unsigned long)-v) : (unsigned long)v;
    do { b[n++] = (char)('0' + (u % 10u)); u /= 10u; } while (u);
    while (n) putch(b[--n]);
}

int dhry_printf(const char *fmt, ...)
{
    /* vbcc m68k: every argument is a 32-bit stack slot right after fmt */
    long *ap = (long *)(&fmt + 1);
    const char *p = fmt;
    while (*p) {
        if (*p != '%') { putch(*p++); continue; }
        p++;
        /* the integrity block uses plain %d/%c/%s only (field widths appear
         * nowhere after the freestanding patch) */
        switch (*p++) {
            case 'd': put_int(*ap++);              break;
            case 'c': putch((int)*ap++);           break;
            case 's': put_str((const char *)*ap++); break;
            case '%': putch('%');                  break;
            default:  putch('?');                  break;
        }
    }
    return 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++) != '\0') ;
    return r;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
