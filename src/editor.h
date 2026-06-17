#pragma once

/*
 * editor.h — the TextEdit window's model.
 *
 * A plain-text editor over a single in-memory buffer: type to insert,
 * Backspace to delete, Enter for a newline, arrows (or a click) to move
 * the caret, Save to write the buffer back to its file on the MicroFS
 * disk. The GUI ([gui.c]) owns the window chrome and feeds keystrokes
 * here whenever the TextEdit window is the focused one.
 */

#include <stdint.h>

struct fs_node;

/* Load a file into the buffer (NULL = an empty, untitled buffer). */
void        editor_open(struct fs_node *n);

/* A typed character: printable, '\b' (backspace), '\n', or '\t'. */
void        editor_key(char c);
/* Move the caret: 0=left 1=right 2=up 3=down. */
void        editor_arrow(int dir);
/* Place the caret from a click, relative to the text area's top-left. */
void        editor_click(int rel_x, int rel_y);

/* Draw the text + caret inside the given rectangle. */
void        editor_draw(int x, int y, int w, int h);

int         editor_copy(char *out, int max);   /* copy buffer out; returns len */
void        editor_paste(const char *s);       /* insert s at the caret */
int         editor_save(void);          /* write buffer to the file; 0 = ok */
int         editor_dirty(void);         /* unsaved changes? */
const char *editor_filename(void);
uint32_t    editor_version(void);       /* bumps on any change (repaint cue) */
