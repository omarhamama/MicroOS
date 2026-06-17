#include <stdarg.h>
#include <stdint.h>
#include "uart.h"
#include "spinlock.h"
#include "kprintf.h"

/*
 * The formatter is split from the destination: kformat() turns a
 * format string into characters and hands each one to an `emit`
 * callback. kprintf() emits to the serial port; ksprintf() emits into
 * a buffer (the GUI draws those). One formatter, any destination —
 * the same shape as Linux's vsnprintf core.
 */

typedef void (*emit_fn)(char c, void *ctx);

/*
 * Convert a number to text by repeatedly dividing by the base and
 * collecting remainders. The digits come out backwards (least
 * significant first), so we build them in a buffer and print in reverse.
 */
static void emit_unsigned(emit_fn emit, void *ctx, unsigned long v, unsigned base,
                          int width, char pad)
{
    static const char digits[] = "0123456789abcdef";
    char buf[24];   /* 2^64 is 20 decimal digits — 24 is plenty */
    int i = 0;

    do {
        buf[i++] = digits[v % base];
        v /= base;
    } while (v);

    for (int k = i; k < width; k++)     /* pad to the requested width */
        emit(pad, ctx);
    while (i--)
        emit(buf[i], ctx);
}

static void emit_signed(emit_fn emit, void *ctx, long v, int width, char pad)
{
    if (v < 0) {
        emit('-', ctx);
        /* Negate carefully via unsigned: -LONG_MIN overflows long. */
        emit_unsigned(emit, ctx, -(unsigned long)v, 10, width - 1, pad);
    } else {
        emit_unsigned(emit, ctx, (unsigned long)v, 10, width, pad);
    }
}

static void kformat(emit_fn emit, void *ctx, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(*fmt, ctx);
            continue;
        }

        fmt++;                      /* skip the '%' */

        /* Optional minimal width with optional '0' pad: %02d, %3u, ... */
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int is_long = 0;
        if (*fmt == 'l') {          /* "long" modifier: %ld %lu %lx */
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
            emit_signed(emit, ctx, is_long ? va_arg(ap, long) : va_arg(ap, int),
                        width, pad);
            break;
        case 'u':
            emit_unsigned(emit, ctx, is_long ? va_arg(ap, unsigned long)
                                             : va_arg(ap, unsigned int), 10,
                          width, pad);
            break;
        case 'x':
            emit_unsigned(emit, ctx, is_long ? va_arg(ap, unsigned long)
                                             : va_arg(ap, unsigned int), 16,
                          width, pad);
            break;
        case 'p':
            emit('0', ctx);
            emit('x', ctx);
            emit_unsigned(emit, ctx, (unsigned long)va_arg(ap, void *), 16, 0, ' ');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                emit(*s++, ctx);
            break;
        }
        case 'c':
            emit((char)va_arg(ap, int), ctx); /* chars promote to int */
            break;
        case '%':
            emit('%', ctx);
            break;
        case '\0':
            return;                 /* string ended right after a '%' */
        default:                    /* unknown conversion: print it raw */
            emit('%', ctx);
            emit(*fmt, ctx);
            break;
        }
    }
}

/* ---- destination 1: the serial console ----------------------------- */

static void uart_emit(char c, void *ctx)
{
    (void)ctx;
    if (c == '\n')
        uart_putc('\r');            /* serial terminals want \r\n */
    uart_putc(c);
}

/* With a second core printing, two kprintfs would interleave mid-line
 * into confetti. One spinlock serializes whole messages — engaged only
 * once SMP starts (uniprocessor pays nothing). Panic mode bypasses it:
 * a crash report must never deadlock on a lock its own core holds. */
static spinlock_t print_lock;
static int locking, panicking;

void kprintf_enable_lock(void) { locking = 1; }
void kprintf_panic_mode(void)  { panicking = 1; }

void kprintf(const char *fmt, ...)
{
    int use_lock = locking && !panicking;
    uint64_t daif = 0;

    if (use_lock) {                 /* no preemption while holding it */
        asm volatile("mrs %0, daif" : "=r"(daif));
        asm volatile("msr daifset, #2");
        spin_lock(&print_lock);
    }

    va_list ap;
    va_start(ap, fmt);
    kformat(uart_emit, 0, fmt, ap);
    va_end(ap);

    if (use_lock) {
        spin_unlock(&print_lock);
        asm volatile("msr daif, %0" :: "r"(daif));
    }
}

/* ---- destination 2: a caller's buffer ------------------------------ */

struct buf_ctx {
    char *p;
    uint64_t left;                  /* room excluding the final NUL */
};

static void buf_emit(char c, void *ctx)
{
    struct buf_ctx *b = ctx;
    if (b->left) {
        *b->p++ = c;
        b->left--;
    }
}

void ksprintf(char *buf, uint64_t size, const char *fmt, ...)
{
    if (!size)
        return;
    struct buf_ctx b = { buf, size - 1 };
    va_list ap;
    va_start(ap, fmt);
    kformat(buf_emit, &b, fmt, ap);
    va_end(ap);
    *b.p = '\0';
}
