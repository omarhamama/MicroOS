#pragma once

#include <stdint.h>

/* Load an ELF program from /bin (or any path) and run it as a new
 * process: its own address space, entered at EL0 with argv. argv is a
 * NULL-terminated array of strings (argv[0] = program name). Returns
 * the new task id, or -1. */
int proc_spawn(const char *path, char **argv);

/* Same, but with explicit stdin/stdout file objects (the shell passes
 * pipe ends here to build `a | b`). The new task owns the references. */
struct file;
int proc_spawn_io(const char *path, char **argv,
                  struct file *in, struct file *out);

/* Build an address space from an ELF image + argv and drop into EL0.
 * Used by proc_spawn (fresh task) and the exec syscall (replace image,
 * passing the caller's old page table to reclaim). Never returns on
 * success; returns -1 on failure with the new table cleaned up. */
int proc_load_and_run(const uint8_t *image, uint64_t size,
                      int argc, char **argv, uint64_t *old_table);

/* Install the user programs that ship inside the kernel into /bin. */
void proc_install_binaries(void);
