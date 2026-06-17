#pragma once

/*
 * usys.h — the user side of the system-call ABI: what a real libc's
 * syscall wrappers do. Numbers MUST match src/syscall.h. The contract:
 * number in x8, args in x0-x2, `svc #0`, result in x0.
 */

#define SYS_EXIT     0   /* exit(code)                — never returns      */
#define SYS_WRITE    1   /* write(fd, buf, len)       -> bytes written     */
#define SYS_READ     2   /* read(fd, buf, len)        -> bytes (0 = EOF)   */
#define SYS_OPEN     3   /* open(path, flags)         -> fd or -1          */
#define SYS_CLOSE    4   /* close(fd)                                      */
#define SYS_SLEEP    5   /* sleep(ticks)                                   */
#define SYS_TICKS    6   /* ticks()                   -> tick counter      */
#define SYS_FORK     7   /* fork()  -> child pid in parent, 0 in child     */
#define SYS_EXEC     8   /* exec(path, argv)          replaces this image  */
#define SYS_WAIT     9   /* wait(&status)             -> reaped child pid  */
#define SYS_GETPID  10   /* getpid()                                       */
#define SYS_DUP2    11   /* dup2(oldfd, newfd)                             */
#define SYS_PIPE    12   /* pipe(int fds[2])          fds[0]=read,1=write  */
#define SYS_READDIR 13   /* readdir(path, i, namebuf) -> type, -1 = end    */

/* open() flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREATE 0x100
#define O_TRUNC  0x200

static inline long sys(long nr, long a0, long a1, long a2)
{
    register long x8 asm("x8") = nr;
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static inline void exit(int code)          { sys(SYS_EXIT, code, 0, 0); }
static inline long write(int fd, const void *b, long n) { return sys(SYS_WRITE, fd, (long)b, n); }
static inline long read(int fd, void *b, long n)        { return sys(SYS_READ, fd, (long)b, n); }
static inline int  open(const char *p, int f)           { return (int)sys(SYS_OPEN, (long)p, f, 0); }
static inline int  close(int fd)           { return (int)sys(SYS_CLOSE, fd, 0, 0); }
static inline void sleep_ticks(long t)     { sys(SYS_SLEEP, t, 0, 0); }
static inline long ticks(void)             { return sys(SYS_TICKS, 0, 0, 0); }
static inline int  fork(void)              { return (int)sys(SYS_FORK, 0, 0, 0); }
static inline int  exec(const char *p, char *const a[]) { return (int)sys(SYS_EXEC, (long)p, (long)a, 0); }
static inline int  wait(int *st)           { return (int)sys(SYS_WAIT, (long)st, 0, 0); }
static inline int  getpid(void)            { return (int)sys(SYS_GETPID, 0, 0, 0); }
static inline int  dup2(int o, int n)      { return (int)sys(SYS_DUP2, o, n, 0); }
static inline int  pipe(int fds[2])        { return (int)sys(SYS_PIPE, (long)fds, 0, 0); }
static inline int  readdir(const char *p, int i, char *nm) { return (int)sys(SYS_READDIR, (long)p, i, (long)nm); }
