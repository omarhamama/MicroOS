#pragma once

#include <stdint.h>

#define FS_NAME_MAX 28      /* longest file/directory name + NUL */

enum fs_type { FS_FILE, FS_DIR };

/*
 * One node = one file or directory. Directories form a tree by linking
 * to their first child; siblings form a chain via `next`:
 *
 *        root
 *         | children
 *         v
 *        docs/ --next--> welcome.txt --next--> (0)
 *         | children
 *         v
 *        roadmap.txt --next--> (0)
 */
struct fs_node {
    char            name[FS_NAME_MAX];
    enum fs_type    type;
    struct fs_node *parent;
    struct fs_node *children;   /* first entry (directories only) */
    struct fs_node *next;       /* next sibling in the parent directory */
    char           *data;       /* file content (files only) */
    uint64_t        size;       /* content bytes used */
    uint64_t        capacity;   /* content bytes allocated */
    uint32_t        ino;        /* this node's inode number on disk */
};

extern struct fs_node *fs_root;

void fs_init(void);                 /* create the empty root */
void fs_populate_defaults(void);    /* starter files for a blank system */

/* Walk a path ("/docs/note.txt", "../a", "b") starting from `from`
 * for relative paths. Returns the node, or 0 if any step fails. */
struct fs_node *fs_resolve(struct fs_node *from, const char *path);

/* For creating/removing: find the DIRECTORY a path points into and
 * copy the final name component into `leaf` (>= FS_NAME_MAX bytes).
 * "docs/new.txt" -> returns the docs node, leaf = "new.txt". */
struct fs_node *fs_resolve_parent(struct fs_node *from, const char *path,
                                  char *leaf);

struct fs_node *fs_create(struct fs_node *dir, const char *name,
                          enum fs_type type);

#define FS_ERR_ROOT      -1     /* refusing to remove the root */
#define FS_ERR_NOT_EMPTY -2     /* directory still has entries */
int fs_unlink(struct fs_node *node);

int  fs_set_content(struct fs_node *file, const char *data, uint64_t len);
void fs_print_path(struct fs_node *node);   /* e.g. "/docs/notes" */
void fs_path(struct fs_node *node, char *buf, int max);  /* same, into a string */

/* File-manager operations: copy/move/delete whole subtrees. */
int  fs_is_descendant(struct fs_node *dir, struct fs_node *node);
void fs_make_unique(struct fs_node *dir, const char *base, char *out, int max);
struct fs_node *fs_copy(struct fs_node *src, struct fs_node *dstdir,
                        const char *newname);
int  fs_delete_recursive(struct fs_node *node);
