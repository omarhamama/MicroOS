/*
 * ulib.c — user-space libc, built on the write() syscall alone.
 *
 * Same code shape as the kernel's lib.c/kprintf.c, but it runs at EL0
 * and reaches the screen only by asking the kernel (write to fd 1).
 * A program can't touch the UART itself — that's the whole point of
 * user mode.
 */

#include <stdarg.h>
#include "usys.h"
#include "ulib.h"

unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

int ustrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int uatoi(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

void *umemset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void puts1(int fd, const char *s)
{
    write(fd, s, (long)ustrlen(s));
}

void puts(const char *s)
{
    puts1(1, s);
}

/* A tiny printf: just enough conversions for our programs, all routed
 * through write(1, ...). One char at a time is fine at this scale. */
static void out(char c) { write(1, &c, 1); }

static void out_unsigned(unsigned long v, int base)
{
    char buf[24];
    int i = 0;
    const char *d = "0123456789abcdef";
    do { buf[i++] = d[v % base]; v /= base; } while (v);
    while (i--) out(buf[i]);
}

void printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out(*fmt); continue; }
        switch (*++fmt) {
        case 's': { const char *s = va_arg(ap, const char *);
                    puts(s ? s : "(null)"); break; }
        case 'd': { int v = va_arg(ap, int);
                    if (v < 0) { out('-'); v = -v; }
                    out_unsigned((unsigned)v, 10); break; }
        case 'u': out_unsigned(va_arg(ap, unsigned), 10); break;
        case 'x': out_unsigned(va_arg(ap, unsigned), 16); break;
        case 'c': out((char)va_arg(ap, int)); break;
        case '%': out('%'); break;
        case '\0': va_end(ap); return;
        default: out('%'); out(*fmt); break;
        }
    }
    va_end(ap);
}
