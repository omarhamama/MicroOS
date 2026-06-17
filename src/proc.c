/*
 * proc.c — the program loader, now reading real ELF executables.
 *
 * Version 1 loaded a "flat binary": the file's bytes were the memory
 * image, copied wholesale to 0x80000000. That works, but it can't
 * tell code from data — so every page had to be writable AND
 * executable, the opposite of W^X.
 *
 * A real executable is an ELF file (Executable and Linkable Format —
 * the same thing `file /bin/ls` reports on Linux). Its header points
 * at a table of PROGRAM HEADERS, each describing one chunk to load:
 * "copy p_filesz bytes from file offset X to virtual address V, then
 * zero up to p_memsz, and make it R / RW / RX per p_flags." The
 * loader's whole job is to walk that table. Now code lands read-only
 * and executable, data lands writable and no-execute — true W^X, told
 * to us by the linker.
 *
 * Loading a program is four honest steps:
 *   1. parse the ELF header + program headers
 *   2. for each PT_LOAD segment: allocate frames, copy bytes, map with
 *      the right permissions into a fresh address space
 *   3. build the user stack with argc/argv (the AArch64 entry ABI)
 *   4. flush the icache (we wrote bytes the CPU will execute) and eret
 *      into EL0 at the ELF's entry point
 */

#include "proc.h"
#include "task.h"
#include "mmu.h"
#include "mem.h"
#include "fs.h"
#include "file.h"
#include "lib.h"
#include "kprintf.h"

#define MAXARG     16
#define ARG_MAX    64
#define MAX_IMAGE  (256 * 1024)

/* ---- ELF64, the parts we use --------------------------------------- */

struct elf_hdr {
    uint8_t  ident[16];     /* 0x7F 'E' 'L' 'F', class, endianness... */
    uint16_t type;          /* 2 = ET_EXEC */
    uint16_t machine;       /* 183 = AArch64 */
    uint32_t version;
    uint64_t entry;         /* where execution begins */
    uint64_t phoff;         /* file offset of the program-header table */
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_phdr {
    uint32_t type;          /* 1 = PT_LOAD */
    uint32_t flags;         /* 1=X 2=W 4=R */
    uint64_t offset;        /* where in the file */
    uint64_t vaddr;         /* where in memory */
    uint64_t paddr;
    uint64_t filesz;        /* bytes to copy from the file */
    uint64_t memsz;         /* bytes in memory (>= filesz; rest is bss) */
    uint64_t align;
};

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2

/* What proc_spawn hands proc_entry: image bytes followed by packed,
 * NUL-terminated argv strings, plus optional stdin/stdout file objects
 * (NULL = the console). The shell sets in/out to pipe ends to build a
 * pipeline. (The shell's argv pointers are transient, so we serialize
 * a private copy here.) */
struct exec_args {
    uint64_t     img_size;
    int          argc;
    struct file *in, *out;
    uint8_t      data[];    /* [image][arg0\0][arg1\0]...] */
};

/* ---- the loader proper --------------------------------------------- */

/* Map one PT_LOAD segment into `table`, page by page. */
static int load_segment(uint64_t *table, const uint8_t *image,
                        const struct elf_phdr *ph)
{
    int perm = (ph->flags & PF_X) ? MMU_PERM_RX
             : (ph->flags & PF_W) ? MMU_PERM_RW
             :                      MMU_PERM_RO;

    uint64_t va_end = ph->vaddr + ph->memsz;
    for (uint64_t va = ph->vaddr & ~0xFFFUL; va < va_end; va += 4096) {
        void *frame = page_alloc();         /* arrives zeroed (= bss) */
        if (!frame)
            return -1;

        /* Which file bytes land in this page? The intersection of the
         * page [va, va+4096) with the file region [vaddr, vaddr+filesz). */
        uint64_t pg_lo = va, pg_hi = va + 4096;
        uint64_t f_lo = ph->vaddr, f_hi = ph->vaddr + ph->filesz;
        uint64_t lo = pg_lo > f_lo ? pg_lo : f_lo;
        uint64_t hi = pg_hi < f_hi ? pg_hi : f_hi;
        if (hi > lo)
            memcpy((uint8_t *)frame + (lo - va),
                   image + ph->offset + (lo - ph->vaddr), hi - lo);

        if (mmu_user_map(table, va, frame, perm) < 0) {
            page_free(frame);
            return -1;
        }
    }
    return 0;
}

/* Validate + load every PT_LOAD segment. Returns the entry point, or 0. */
static uint64_t load_elf(uint64_t *table, const uint8_t *image, uint64_t size)
{
    if (size < sizeof(struct elf_hdr))
        return 0;
    const struct elf_hdr *eh = (const struct elf_hdr *)image;

    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L'  || eh->ident[3] != 'F') {
        kprintf("exec: not an ELF file\n");
        return 0;
    }
    if (eh->ident[4] != 2 /* 64-bit */ || eh->machine != 183 /* AArch64 */ ||
        eh->type != 2 /* ET_EXEC */) {
        kprintf("exec: wrong ELF class/machine/type\n");
        return 0;
    }

    for (int i = 0; i < eh->phnum; i++) {
        const struct elf_phdr *ph = (const struct elf_phdr *)
            (image + eh->phoff + (uint64_t)i * eh->phentsize);
        if ((const uint8_t *)ph + sizeof(*ph) > image + size)
            return 0;
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz > USER_STACK_VA ||
            ph->offset + ph->filesz > size)
            return 0;                       /* refuses to map over the stack */
        if (load_segment(table, image, ph) < 0)
            return 0;
    }
    return eh->entry;
}

/* Build the user stack: copy the argv strings near the top, then an
 * array of pointers to them (NULL-terminated). Returns the stack
 * pointer; *argv_out gets the user address of the pointer array. Must
 * run AFTER the new table is active (it writes user virtual addresses). */
static uint64_t setup_stack(int argc, char **argv, uint64_t *argv_out)
{
    uint64_t sp = USER_STACK_TOP;
    uint64_t uptr[MAXARG];

    for (int i = 0; i < argc; i++) {
        uint64_t len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        uptr[i] = sp;
    }

    sp &= ~15UL;
    sp -= (uint64_t)(argc + 1) * 8;         /* the argv[] array + NULL */
    sp &= ~15UL;                            /* ABI: 16-byte aligned sp */

    uint64_t *ua = (uint64_t *)sp;
    for (int i = 0; i < argc; i++)
        ua[i] = uptr[i];
    ua[argc] = 0;

    *argv_out = sp;
    return sp;
}

/* The one-way door into EL0, with argc/argv in place. */
static void enter_el0(uint64_t pc, uint64_t sp, uint64_t argc, uint64_t argv)
{
    register uint64_t x0 asm("x0") = argc;
    register uint64_t x1 asm("x1") = argv;
    asm volatile(
        "msr sp_el0,   %0\n"
        "msr elr_el1,  %1\n"
        "msr spsr_el1, xzr\n"               /* EL0, SP_EL0, IRQs on */
        "eret\n"
        :: "r"(sp), "r"(pc), "r"(x0), "r"(x1) : "memory");
    __builtin_unreachable();
}

/* Build a process's whole address space from an ELF image + argv, then
 * drop into it. Shared by proc_spawn (a brand-new task) and the exec
 * syscall (replacing the caller's image). On success, never returns. */
int proc_load_and_run(const uint8_t *image, uint64_t size,
                      int argc, char **argv, uint64_t *old_table)
{
    uint64_t *table = mmu_user_table_new();
    if (!table)
        return -1;

    uint64_t entry = load_elf(table, image, size);
    if (!entry)
        goto fail;

    for (uint64_t va = USER_STACK_VA; va < USER_STACK_TOP; va += 4096) {
        void *frame = page_alloc();
        if (!frame || mmu_user_map(table, va, frame, MMU_PERM_RW) < 0)
            goto fail;
    }

    current->pgtable = table;
    mmu_switch(table);                      /* enter the new world... */
    if (old_table)
        mmu_user_table_free(old_table);     /* ...and reclaim the old one */

    asm volatile("dsb ish; ic iallu; dsb ish; isb");    /* icache: see below */

    uint64_t uargv, sp = setup_stack(argc, argv, &uargv);
    enter_el0(entry, sp, (uint64_t)argc, uargv);        /* never returns */

fail:
    mmu_user_table_free(table);
    return -1;
}

/* A fresh task's first run: give it standard fds, unpack argv, go. */
static void proc_entry(uint64_t arg)
{
    struct exec_args *xa = (struct exec_args *)arg;

    /* stdin/stdout from the shell (a pipe end, maybe); stderr always
     * the console. Whatever the shell didn't specify defaults to the
     * console. The shell handed us its references, so we don't dup. */
    current->ofile[0] = xa->in  ? xa->in  : file_console();
    current->ofile[1] = xa->out ? xa->out : file_console();
    current->ofile[2] = file_console();

    char *argv[MAXARG];
    char *p = (char *)xa->data + xa->img_size;
    for (int i = 0; i < xa->argc; i++) {
        argv[i] = p;
        p += strlen(p) + 1;
    }

    int rc = proc_load_and_run(xa->data, xa->img_size, xa->argc, argv, 0);
    kfree(xa);
    if (rc < 0)
        kprintf("exec: failed to load program\n");
    task_exit();                            /* only reached on failure */
}

/* ------------------------------------------------------------------ */
/* fork — the most magical syscall: one call, two returns.             */
/* ------------------------------------------------------------------ */
/*
 * fork() duplicates the calling process. We:
 *   1. clone the address space (every user page copied — mmu_user_copy)
 *   2. clone the open-file table (dup, so both share file offsets/pipes)
 *   3. hand the child a COPY of the parent's trap frame, but with x0=0,
 *      parked on the child's kernel stack so that when the scheduler
 *      first runs it, ret_to_user erets straight back to EL0 — to the
 *      instruction after `svc`, exactly where the parent is going too.
 * The parent returns the child's pid; the child returns 0. Same code,
 * two universes — that's how every Unix process tree grows.
 */
int proc_fork(struct trap_frame *pf)
{
    struct task *child = task_alloc_child(current->name);
    if (!child)
        return -1;

    child->pgtable = mmu_user_table_new();
    if (!child->pgtable || mmu_user_copy(child->pgtable, current->pgtable) < 0) {
        if (child->pgtable)
            mmu_user_table_free(child->pgtable);
        child->pgtable = 0;
        child->state = TASK_ZOMBIE;     /* let the slot be reclaimed */
        return -1;
    }

    for (int i = 0; i < NOFILE; i++)
        if (current->ofile[i])
            child->ofile[i] = file_dup(current->ofile[i]);

    /* Park a trap frame at the top of the child's kernel stack. */
    uint64_t top = (uint64_t)(child->stack + TASK_STACK_SIZE) & ~15UL;
    struct trap_frame *cf = (struct trap_frame *)(top - sizeof(*cf));
    *cf = *pf;                          /* same user state as the parent */
    cf->x[0] = 0;                       /* ...except fork() returns 0 here */

    memset(&child->ctx, 0, sizeof(child->ctx));
    child->ctx.sp = (uint64_t)cf;
    child->ctx.lr = (uint64_t)ret_to_user;
    child->state = TASK_RUNNABLE;       /* now the scheduler may run it */

    return child->id;                   /* the parent's return value */
}

/* ------------------------------------------------------------------ */
/* exec — replace THIS process's image with a new program.            */
/* ------------------------------------------------------------------ */
/*
 * Unlike fork, exec keeps the same process (same pid, same fd table —
 * which is exactly why a shell can set up redirection, then exec).
 * Only the address space changes. We copy argv out of user memory
 * first (we're about to demolish the address space it lives in), then
 * build the new world and drop into it — never returning here on
 * success.
 */
int proc_exec_syscall(struct trap_frame *f, const char *path, char **argv)
{
    (void)f;
    struct fs_node *fnode = fs_resolve(fs_root, path);
    if (!fnode || fnode->type != FS_FILE || !fnode->size ||
        fnode->size > MAX_IMAGE)
        return -1;

    /* Copy argv (user pointers -> user strings) into kernel memory
     * BEFORE we free the address space they live in. */
    static char kbuf[MAXARG][ARG_MAX];
    char *kargv[MAXARG];
    int argc = 0;
    if (argv) {
        while (argc < MAXARG && argv[argc]) {
            const char *s = argv[argc];
            int j = 0;
            while (j < ARG_MAX - 1 && s[j]) {
                kbuf[argc][j] = s[j];
                j++;
            }
            kbuf[argc][j] = '\0';
            kargv[argc] = kbuf[argc];
            argc++;
        }
    }
    kargv[argc] = 0;

    /* fnode->data is kernel heap (mapped in every address space), so it
     * survives the switch. Hand our current table over to be freed. */
    uint64_t *old = current->pgtable;
    proc_load_and_run((const uint8_t *)fnode->data, fnode->size,
                      argc, kargv, old);   /* never returns on success */
    return -1;                              /* load failed; image intact */
}

/* Serialize image + argv into one blob and start a task to run it,
 * with stdin=in and stdout=out (NULL = console). The new task takes
 * ownership of the in/out file references. */
int proc_spawn_io(const char *path, char **argv,
                  struct file *in, struct file *out)
{
    /* We OWN in/out: on any failure we must close them, or a pipe end
     * leaks and its partner hangs forever waiting for EOF. */
    struct fs_node *f = fs_resolve(fs_root, path);
    if (!f || f->type != FS_FILE || !f->size || f->size > MAX_IMAGE) {
        file_close(in);
        file_close(out);
        return -1;
    }

    int argc = 0;
    uint64_t argbytes = 0;
    while (argc < MAXARG && argv[argc]) {
        argbytes += strlen(argv[argc]) + 1;
        argc++;
    }

    struct exec_args *xa = kmalloc(sizeof(*xa) + f->size + argbytes);
    if (!xa) {
        file_close(in);
        file_close(out);
        return -1;
    }
    xa->img_size = f->size;
    xa->argc = argc;
    xa->in = in;
    xa->out = out;
    memcpy(xa->data, f->data, f->size);

    char *p = (char *)xa->data + f->size;
    for (int i = 0; i < argc; i++) {
        uint64_t len = strlen(argv[i]) + 1;
        memcpy(p, argv[i], len);
        p += len;
    }

    int id = task_create(argc ? argv[0] : path, proc_entry, (uint64_t)xa);
    if (id < 0) {
        kfree(xa);
        file_close(in);
        file_close(out);
    }
    return id;
}

/* The common case: run a program with the console for stdin/stdout. */
int proc_spawn(const char *path, char **argv)
{
    return proc_spawn_io(path, argv, 0, 0);
}

/* ------------------------------------------------------------------ */
/* User programs ship inside the kernel image, installed to /bin at    */
/* boot. They are real ELF files (the .uelf the Makefile builds).      */
/* ------------------------------------------------------------------ */

#define DECL(p)  extern char _binary_##p##_uelf_start[], _binary_##p##_uelf_end[]
DECL(hello); DECL(rogue); DECL(echo); DECL(cat); DECL(ls); DECL(forkdemo);
DECL(wc); DECL(pipedemo); DECL(lisp);

static void install(const char *path, const char *data, uint64_t len)
{
    char leaf[FS_NAME_MAX];
    struct fs_node *f = fs_resolve(fs_root, path);
    if (!f) {
        struct fs_node *dir = fs_resolve_parent(fs_root, path, leaf);
        if (dir)
            f = fs_create(dir, leaf, FS_FILE);
    }
    if (f)
        fs_set_content(f, data, len);
}

#define INSTALL(name) install("/bin/" #name, _binary_##name##_uelf_start, \
    (uint64_t)(_binary_##name##_uelf_end - _binary_##name##_uelf_start))

void proc_install_binaries(void)
{
    if (!fs_resolve(fs_root, "/bin"))
        fs_create(fs_root, "bin", FS_DIR);

    INSTALL(hello);
    INSTALL(rogue);
    INSTALL(echo);
    INSTALL(cat);
    INSTALL(ls);
    INSTALL(forkdemo);
    INSTALL(wc);
    INSTALL(pipedemo);
    INSTALL(lisp);

    kprintf("[boot] installed ELF programs in /bin "
            "(try: exec /bin/hello a b c)\n");
}
