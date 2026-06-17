/*
 * forkdemo.c — the Unix process trinity: fork, exec, wait.
 *
 * fork() splits this program into two. The child execs a different
 * program (/bin/echo); the parent waits for it to finish and reads its
 * exit status. This three-step — fork to make a process, exec to give
 * it a new program, wait to collect it — is exactly how a shell runs
 * every command you type.
 */

#include "usys.h"
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("(forkdemo) I am pid %d. Calling fork()...\n", getpid());

    int pid = fork();
    if (pid < 0) {
        printf("(forkdemo) fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* The child: same code, but fork() returned 0 here. */
        printf("(forkdemo) child (pid %d): now exec'ing /bin/echo\n", getpid());
        char *a[] = { "/bin/echo", "hello", "from", "the", "child", 0 };
        exec("/bin/echo", a);
        printf("(forkdemo) child: exec failed!\n");   /* only if exec fails */
        return 2;
    }

    /* The parent: fork() returned the child's pid. */
    printf("(forkdemo) parent (pid %d): forked child %d, waiting for it\n",
           getpid(), pid);
    int status;
    int reaped = wait(&status);
    printf("(forkdemo) parent: child %d finished with status %d\n",
           reaped, status);
    return 0;
}
