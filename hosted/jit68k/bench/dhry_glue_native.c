/* dhry_glue_native.c — native arm64-darwin counterpart of dhry_glue.c.
 * Same services, same naive strcpy/strcmp (the 68k build has no libc SIMD
 * string routines, so using the identical byte loops keeps the workload
 * comparable); only the vararg mechanism differs (AAPCS stdarg vs vbcc's
 * 32-bit stack slots). */

#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

static char obuf[8192];
static int  olen;

static void putch(int c)
{
    obuf[olen++] = (char)c;
    if (olen == (int)sizeof obuf) { write(1, obuf, olen); olen = 0; }
}

void dhry_flush(void) { if (olen) { write(1, obuf, olen); olen = 0; } }

__attribute__((constructor)) static void dhry_flush_at_exit(void) { atexit(dhry_flush); }

static void put_str(const char *s) { while (*s) putch(*s++); }

static void put_int(long v)
{
    char b[24]; int n = 0;
    unsigned long u = (v < 0) ? (putch('-'), (unsigned long)-v) : (unsigned long)v;
    do { b[n++] = (char)('0' + (u % 10u)); u /= 10u; } while (u);
    while (n) putch(b[--n]);
}

int dhry_printf(const char *fmt, ...)
{
    va_list ap;
    const char *p = fmt;
    va_start(ap, fmt);
    while (*p) {
        if (*p != '%') { putch(*p++); continue; }
        p++;
        switch (*p++) {
            case 'd': put_int(va_arg(ap, int));                 break;
            case 'c': putch(va_arg(ap, int));                   break;
            case 's': put_str(va_arg(ap, const char *));        break;
            case '%': putch('%');                               break;
            default:  putch('?');                               break;
        }
    }
    va_end(ap);
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
