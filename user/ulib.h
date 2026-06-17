#pragma once

/* A minimal user-space C library. On a real system this is the part of
 * libc that ISN'T syscall wrappers: string helpers and printf, built
 * entirely on top of the write()/read() syscalls in usys.h. */

unsigned long ustrlen(const char *s);
int   ustrcmp(const char *a, const char *b);
int   uatoi(const char *s);
void *umemset(void *d, int c, unsigned long n);

void  puts1(int fd, const char *s);     /* write a string to a fd */
void  puts(const char *s);              /* ...to stdout */
void  printf(const char *fmt, ...);     /* %s %d %u %x %c, to stdout */
