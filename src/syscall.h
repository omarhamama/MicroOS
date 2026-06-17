#pragma once

/* The MicroOS system-call ABI. Numbers MUST match user/usys.h.
 * Convention: number in x8, args in x0-x2, `svc #0`, result in x0. */
#define SYS_EXIT     0
#define SYS_WRITE    1   /* write(fd, buf, len)       */
#define SYS_READ     2   /* read(fd, buf, len)        */
#define SYS_OPEN     3   /* open(path, flags)         */
#define SYS_CLOSE    4   /* close(fd)                 */
#define SYS_SLEEP    5
#define SYS_TICKS    6
#define SYS_FORK     7
#define SYS_EXEC     8   /* exec(path, argv)          */
#define SYS_WAIT     9   /* wait(&status)             */
#define SYS_GETPID  10
#define SYS_DUP2    11   /* dup2(oldfd, newfd)        */
#define SYS_PIPE    12   /* pipe(int fds[2])          */
#define SYS_READDIR 13   /* readdir(path, idx, name)  */
