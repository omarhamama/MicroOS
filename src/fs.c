/*
 * fs.c — a RAM filesystem (everything lives in kmalloc'd memory).
 *
 * "Filesystem" sounds like it's about disks, but the heart of one is a
 * NAMESPACE: turning a human name like "/docs/note.txt" into the actual
 * bytes. That problem is the same whether the bytes live in RAM or on
 * an SSD — so we learn it here first, with no disk driver in the way.
 * Linux does the same thing: tmpfs (/tmp on many systems) is exactly
 * this, files in kernel memory.
 *
 * The pieces map one-to-one onto real kernel concepts:
 *
 *   struct fs_node     ~ Linux's "inode" (the file itself) and
 *                        "dentry" (its name in a directory) fused into
 *                        one struct for simplicity
 *   fs_resolve()       ~ the path walk Linux does for every open()
 *   fs_unlink()        ~ unlink()/rmdir() — what `rm` ultimately calls
 *
 * Persistence lives one layer down: virtio_blk.c (the disk) and
 * diskfs.c (the on-disk format) save and restore this tree. This file
 * neither knows nor cares — the same separation real kernels keep
 * between the VFS layer and individual filesystems.
 *
 * What we DON'T have yet, and where to find the lesson:
 *   - permissions  -> needs user mode first; nothing to protect yet
 *   - open()/fd's  -> needs processes; "which files has THIS task open"
 */

#include "fs.h"
#include "mfs.h"
#include "mem.h"
#include "lib.h"
#include "kprintf.h"
#include "sample_mp3.h"     /* a real .mp3 to ship in /music for the player */
#include "sample_img.h"     /* a PNG + JPEG to ship in /pictures for Preview */

struct fs_node *fs_root;

static struct fs_node *find_child(struct fs_node *dir, const char *name)
{
    for (struct fs_node *c = dir->children; c; c = c->next)
        if (strcmp(c->name, name) == 0)
            return c;
    return 0;
}

/*
 * The path walk: chop the path into '/'-separated components and
 * descend one directory at a time. "." means "stay", ".." means "go
 * up" — they're not magic, just names the walker treats specially.
 */
struct fs_node *fs_resolve(struct fs_node *from, const char *path)
{
    /* An absolute path (leading '/') restarts the walk at the root. */
    struct fs_node *n = (*path == '/') ? fs_root : from;
    char comp[FS_NAME_MAX];

    while (*path) {
        while (*path == '/')            /* skip slashes (also "a//b") */
            path++;
        if (!*path)
            break;

        int i = 0;
        while (*path && *path != '/') {
            if (i >= FS_NAME_MAX - 1)
                return 0;               /* component name too long */
            comp[i++] = *path++;
        }
        comp[i] = '\0';

        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            if (n->parent)              /* ".." at the root stays put */
                n = n->parent;
            continue;
        }

        if (n->type != FS_DIR)
            return 0;                   /* "file.txt/x" makes no sense */
        n = find_child(n, comp);
        if (!n)
            return 0;                   /* no such name */
    }
    return n;
}

struct fs_node *fs_resolve_parent(struct fs_node *from, const char *path,
                                  char *leaf)
{
    /* Split at the LAST slash: everything before names the directory,
     * everything after is the new entry's name. */
    int last = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/')
            last = i;

    const char *name = (last < 0) ? path : path + last + 1;
    if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    if (strlen(name) >= FS_NAME_MAX)
        return 0;

    for (int i = 0; (leaf[i] = name[i]) != '\0'; i++) { }

    if (last < 0)                       /* "note.txt" — right here */
        return from;
    if (last == 0)                      /* "/note.txt" — in the root */
        return fs_root;

    char dirpath[112];
    if ((unsigned)last >= sizeof(dirpath))
        return 0;
    memcpy(dirpath, path, (uint64_t)last);
    dirpath[last] = '\0';

    struct fs_node *dir = fs_resolve(from, dirpath);
    return (dir && dir->type == FS_DIR) ? dir : 0;
}

struct fs_node *fs_create(struct fs_node *dir, const char *name,
                          enum fs_type type)
{
    if (dir->type != FS_DIR || strlen(name) >= FS_NAME_MAX)
        return 0;
    if (find_child(dir, name))
        return 0;                       /* name already taken */

    struct fs_node *n = kmalloc(sizeof(*n));
    if (!n)
        return 0;
    memset(n, 0, sizeof(*n));
    for (int i = 0; (n->name[i] = name[i]) != '\0'; i++) { }
    n->type = type;
    n->parent = dir;

    /* Append at the tail so `ls` shows creation order. */
    struct fs_node **pp = &dir->children;
    while (*pp)
        pp = &(*pp)->next;
    *pp = n;

    /* Write-through: the new entry goes to disk before we return. If
     * the disk says no (out of inodes?), undo the RAM side too — the
     * cache must never show what the disk doesn't have. */
    if (mfs_on_create(n) < 0) {
        *pp = 0;
        kfree(n);
        return 0;
    }

    return n;
}

int fs_unlink(struct fs_node *n)
{
    if (!n->parent)
        return FS_ERR_ROOT;
    if (n->type == FS_DIR && n->children)
        return FS_ERR_NOT_EMPTY;        /* like rmdir: empty dirs only */

    /* Detach from the parent's child chain... */
    struct fs_node **pp = &n->parent->children;
    while (*pp && *pp != n)
        pp = &(*pp)->next;
    if (*pp)
        *pp = n->next;

    /* ...free its inode and blocks on disk (write-through)... */
    mfs_on_unlink(n);

    /* ...and give the memory back. This is why kfree() had to exist
     * before the filesystem could. */
    kfree(n->data);
    kfree(n);
    return 0;
}

int fs_set_content(struct fs_node *f, const char *data, uint64_t len)
{
    if (f->type != FS_FILE)
        return -1;
    if (len > MFS_MAX_FILE)
        return -1;      /* 24 direct blocks; indirect blocks = exercise */

    if (len > f->capacity) {            /* outgrown the old buffer */
        char *fresh = kmalloc(len);
        if (!fresh)
            return -1;
        kfree(f->data);
        f->data = fresh;
        f->capacity = len;
    }
    memcpy(f->data, data, len);
    f->size = len;

    return mfs_on_content(f);           /* write-through to disk */
}

void fs_print_path(struct fs_node *n)
{
    if (!n->parent) {                   /* the root is just "/" */
        kprintf("/");
        return;
    }
    fs_print_path(n->parent);
    if (n->parent->parent)              /* no double slash after root */
        kprintf("/");
    kprintf("%s", n->name);
}

/* Same path, into a string (for the GUI file manager's breadcrumb). */
static int fs_path_rec(struct fs_node *n, char *buf, int pos, int max)
{
    if (!n->parent) {
        if (pos < max - 1)
            buf[pos++] = '/';
        return pos;
    }
    pos = fs_path_rec(n->parent, buf, pos, max);
    if (n->parent->parent && pos < max - 1)
        buf[pos++] = '/';
    for (int i = 0; n->name[i] && pos < max - 1; i++)
        buf[pos++] = n->name[i];
    return pos;
}

void fs_path(struct fs_node *n, char *buf, int max)
{
    int pos = fs_path_rec(n, buf, 0, max);
    buf[pos < max ? pos : max - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Higher-level operations a file manager needs: copy, move, delete.   */
/* ------------------------------------------------------------------ */

/* Is `node` the same as `dir`, or somewhere inside dir's subtree?
 * Used to forbid pasting/moving a folder into itself (which would
 * recurse forever and corrupt the tree). */
int fs_is_descendant(struct fs_node *dir, struct fs_node *node)
{
    for (struct fs_node *p = node; p; p = p->parent)
        if (p == dir)
            return 1;
    return 0;
}

/* Find a name not already taken in `dir`: "report", then "report_copy",
 * "report_copy2", ... — like Finder appending "copy". */
void fs_make_unique(struct fs_node *dir, const char *base, char *out, int max)
{
    int n = 0;
    while (base[n] && n < max - 1) { out[n] = base[n]; n++; }
    out[n] = '\0';
    if (!find_child(dir, out))
        return;
    for (int i = 1; i < 100; i++) {
        if (i == 1) ksprintf(out, (unsigned long)max, "%s_copy", base);
        else        ksprintf(out, (unsigned long)max, "%s_copy%d", base, i);
        if (!find_child(dir, out))
            return;
    }
}

/* Deep-copy `src` into `dstdir` as `newname` (recursively for folders).
 * Each fs_create / fs_set_content writes through to disk, so the copy
 * is persisted exactly like one made by hand. Returns the new node. */
struct fs_node *fs_copy(struct fs_node *src, struct fs_node *dstdir,
                        const char *newname)
{
    if (src->type == FS_FILE) {
        struct fs_node *f = fs_create(dstdir, newname, FS_FILE);
        if (f && src->size)
            fs_set_content(f, src->data, src->size);
        return f;
    }
    struct fs_node *d = fs_create(dstdir, newname, FS_DIR);
    if (!d)
        return 0;
    for (struct fs_node *c = src->children; c; c = c->next)
        if (!fs_copy(c, d, c->name))    /* a child failed (out of space) */
            return 0;
    return d;
}

/* Delete a node and everything under it. (fs_unlink alone refuses a
 * non-empty directory, like rmdir — so we empty it depth-first first.)
 * MOVE is just fs_copy then this on the original. */
int fs_delete_recursive(struct fs_node *node)
{
    if (!node->parent)
        return FS_ERR_ROOT;
    while (node->children)              /* empty it before removing it */
        fs_delete_recursive(node->children);
    return fs_unlink(node);
}

/* ------------------------------------------------------------------ */

static void add_file(struct fs_node *dir, const char *name, const char *text)
{
    struct fs_node *f = fs_create(dir, name, FS_FILE);
    if (f)
        fs_set_content(f, text, strlen(text));
}

void fs_init(void)
{
    fs_root = kmalloc(sizeof(*fs_root));
    memset(fs_root, 0, sizeof(*fs_root));
    fs_root->name[0] = '/';
    fs_root->type = FS_DIR;
}

/* Starter content for a brand-new system (blank or missing disk).
 * When a filesystem is loaded from disk, none of this runs — your
 * files, including any edits to these, are whatever you saved. */
void fs_populate_defaults(void)
{
    add_file(fs_root, "welcome.txt",
        "Welcome to MicroOS — running MicroFS v2.\n"
        "\n"
        "Files hit the disk THE MOMENT you change them: every write\n"
        "allocates real disk blocks, updates an inode, and rewrites\n"
        "the parent directory's name table — just like a grown-up\n"
        "filesystem. The brutal test:\n"
        "\n"
        "    write proof.txt no sync no shutdown\n"
        "    (kill QEMU with Ctrl-A X right now - pull the plug!)\n"
        "    make run\n"
        "    cat proof.txt      <- still here.\n"
        "\n"
        "Run 'disk' to see the superblock, bitmap and inode usage.\n");

    struct fs_node *docs = fs_create(fs_root, "docs", FS_DIR);
    if (docs)
        add_file(docs, "commands.txt",
            "Cheat sheet.\n"
            "\n"
            "Shell builtins (run inside the shell):\n"
            "  tree [path]   cd <path>   pwd   mkdir <path>\n"
            "  write <path> text   rm <path>   disk   help\n"
            "\n"
            "Programs in /bin (real ELF processes the shell launches):\n"
            "  ls [path]          list a directory\n"
            "  cat <path>...      print files (or stdin)\n"
            "  echo <text>        print the arguments\n"
            "  wc [-l]            count lines/words/bytes of stdin\n"
            "  hello / forkdemo / pipedemo   little demos\n"
            "\n"
            "Pipes join programs: cat /welcome.txt | wc -l\n"
            "Run a program by name (found in /bin) or by full path.\n");

    add_file(fs_root, "notes.txt",
        "Scratch pad — this opens in the TextEdit window.\n"
        "\n"
        "Type to insert, Backspace to delete, Enter for a new line,\n"
        "arrow keys or a click to move the caret, then press Save.\n"
        "It is written straight to the MicroFS disk, so it survives a\n"
        "reboot. Double-click any file in the Files window to edit it.\n");

    /* A real MP3 for the music player to decode (open the Music window
     * and pick track 2). Its decoder is in mp3.c. */
    struct fs_node *music = fs_create(fs_root, "music", FS_DIR);
    if (music) {
        struct fs_node *s = fs_create(music, "sample.mp3", FS_FILE);
        if (s)
            fs_set_content(s, (const char *)sample_mp3, sample_mp3_len);
    }

    /* Pictures for the Preview app (double-click them in Files). */
    struct fs_node *pics = fs_create(fs_root, "pictures", FS_DIR);
    if (pics) {
        struct fs_node *p = fs_create(pics, "sample.png", FS_FILE);
        if (p) fs_set_content(p, (const char *)sample_png, sample_png_len);
        struct fs_node *j = fs_create(pics, "photo.jpg", FS_FILE);
        if (j) fs_set_content(j, (const char *)sample_jpg, sample_jpg_len);
    }
}
