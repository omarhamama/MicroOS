/*
 * ls.c — list a directory, using the readdir() system call.
 *
 * The kernel knows the directory layout; ls just asks for entry 0, 1,
 * 2, ... until readdir says "no more" (-1). Each call reports whether
 * the entry is a file or a directory, which we mark with a trailing
 * slash — the same convention `ls -F` uses.
 */

#include "usys.h"
#include "ulib.h"

int main(int argc, char **argv)
{
    const char *path = "/";
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')          /* skip flags like -l for now */
            path = argv[i];

    char name[28];
    int any = 0;
    for (int i = 0; ; i++) {
        int type = readdir(path, i, name);
        if (type < 0)
            break;
        any = 1;
        printf("  %s%s\n", name, type == 1 ? "/" : "");
    }
    if (!any)
        printf("ls: %s is empty or not a directory\n", path);
    return 0;
}
