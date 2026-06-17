#pragma once

/*
 * kprintf — printf for the kernel.
 *
 * There is no C standard library in a kernel (no printf, no malloc —
 * those are built ON TOP of an OS, and we ARE the OS). So we write our
 * own. Supported conversions:
 *
 *   %s  string        %c  char       %%  literal '%'
 *   %d  int           %u  unsigned   %x  unsigned hex
 *   %ld %lu %lx       64-bit variants
 *   %p  pointer (printed as 0x...)
 */
void kprintf(const char *fmt, ...);

/* Same formatter, but into a buffer (always NUL-terminated). */
void ksprintf(char *buf, unsigned long size, const char *fmt, ...);

void kprintf_enable_lock(void);     /* once a second core can print */
void kprintf_panic_mode(void);      /* crashing: skip the lock forever */
