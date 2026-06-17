/*
 * editor.c — a small plain-text editor.
 *
 * The model is deliberately the simplest thing that works: ONE flat byte
 * buffer plus a caret index. Inserting a character memmoves the tail one
 * byte right and drops the new byte in at the caret; Backspace memmoves
 * it one byte left. O(n) per keystroke, but n is a few KB and a human
 * types a few characters a second — it's instant. Newlines are just '\n'
 * bytes in the buffer; there is no separate line array. Everything the
 * editor needs (the line a byte is on, the column, where a click lands)
 * is recomputed by scanning for '\n', which keeps the state impossible to
 * desync.
 *
 * Rendering is a monospace grid (the 8x8 system font): walk the buffer a
 * line at a time, clip each line to the window width, and draw a caret
 * bar at the cursor's row/column. Tab is "soft" — typing it inserts four
 * spaces — so the buffer is always pure text and the columns never lie.
 */

#include "editor.h"
#include "fs.h"
#include "fb.h"
#include "lib.h"
#include "kprintf.h"

#define EBUF_MAX   32768
#define TAB_WIDTH  4
#define CH_W       8                 /* 8x8 font cell */
#define LN_H       12

#define COL_TEXT   0x1d1d1f
#define COL_CARET  0x007aff
#define COL_PANEL  0xffffff

static char  ebuf[EBUF_MAX];
static int   elen;                   /* bytes used */
static int   ecur;                   /* caret position [0..elen] */
static int   escroll;                /* first visible line */
static int   edirty;
static struct fs_node *enode;        /* file backing the buffer (or NULL) */
static char  efname[FS_NAME_MAX];
static volatile uint32_t ever;       /* repaint cue for the GUI */

uint32_t    editor_version(void)  { return ever; }
int         editor_dirty(void)    { return edirty; }
const char *editor_filename(void) { return efname; }

void editor_open(struct fs_node *n)
{
    enode = n;
    elen = 0;
    if (n && n->data) {
        elen = (int)(n->size < EBUF_MAX - 1 ? n->size : EBUF_MAX - 1);
        memcpy(ebuf, n->data, (size_t)elen);
    }
    ecur = 0;
    escroll = 0;
    edirty = 0;
    int i = 0;
    if (n) { for (; n->name[i] && i < FS_NAME_MAX - 1; i++) efname[i] = n->name[i]; }
    else   { const char *u = "(untitled)"; for (; u[i]; i++) efname[i] = u[i]; }
    efname[i] = '\0';
    ever++;
}

/* Byte index of the start of the line containing position p. */
static int line_start(int p)
{
    while (p > 0 && ebuf[p - 1] != '\n') p--;
    return p;
}
/* Byte index of the '\n' ending p's line (or elen if it's the last). */
static int line_end(int p)
{
    while (p < elen && ebuf[p] != '\n') p++;
    return p;
}

static void insert_char(char c)
{
    if (elen >= EBUF_MAX - 1) return;
    memmove(ebuf + ecur + 1, ebuf + ecur, (size_t)(elen - ecur));
    ebuf[ecur] = c;
    elen++;
    ecur++;
    edirty = 1;
}

void editor_key(char c)
{
    if (c == '\b') {                            /* backspace */
        if (ecur > 0) {
            memmove(ebuf + ecur - 1, ebuf + ecur, (size_t)(elen - ecur));
            elen--;
            ecur--;
            edirty = 1;
        }
    } else if (c == '\t') {                      /* soft tab: four spaces */
        for (int i = 0; i < TAB_WIDTH; i++) insert_char(' ');
    } else if (c == '\n' || (c >= ' ' && c < 0x7F)) {
        insert_char(c);
    }
    ever++;
}

void editor_arrow(int dir)
{
    if (dir == 0) {                              /* left */
        if (ecur > 0) ecur--;
    } else if (dir == 1) {                       /* right */
        if (ecur < elen) ecur++;
    } else if (dir == 2) {                       /* up */
        int ls = line_start(ecur);
        if (ls > 0) {
            int col = ecur - ls;
            int pls = line_start(ls - 1);
            int plen = (ls - 1) - pls;
            ecur = pls + (col < plen ? col : plen);
        }
    } else {                                     /* down */
        int ls = line_start(ecur);
        int le = line_end(ecur);
        if (le < elen) {
            int col = ecur - ls;
            int nls = le + 1;
            int nlen = line_end(nls) - nls;
            ecur = nls + (col < nlen ? col : nlen);
        }
    }
    ever++;
}

void editor_click(int rx, int ry)
{
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    int target_line = escroll + ry / LN_H;
    int target_col  = rx / CH_W;

    /* walk to the start of target_line */
    int p = 0, line = 0;
    while (line < target_line && p < elen) {
        if (ebuf[p] == '\n') line++;
        p++;
    }
    /* advance up to target_col within the line */
    int le = line_end(p);
    int col = 0;
    while (p < le && col < target_col) { p++; col++; }
    ecur = p;
    ever++;
}

/* Caret line/column (by scanning), used for autoscroll + drawing. */
static void caret_rc(int *row, int *col)
{
    int r = 0, c = 0;
    for (int i = 0; i < ecur; i++) {
        if (ebuf[i] == '\n') { r++; c = 0; }
        else c++;
    }
    *row = r;
    *col = c;
}

void editor_draw(int x, int y, int w, int h)
{
    int cols = w / CH_W;
    int rows = h / LN_H;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    int crow, ccol;
    caret_rc(&crow, &ccol);
    if (crow < escroll)            escroll = crow;          /* autoscroll */
    if (crow >= escroll + rows)    escroll = crow - rows + 1;
    if (escroll < 0)               escroll = 0;

    /* skip to the first visible line */
    int i = 0, line = 0;
    while (line < escroll && i < elen) {
        if (ebuf[i] == '\n') line++;
        i++;
    }

    char tmp[160];
    for (int r = 0; r < rows && i <= elen; r++) {
        int n = 0;
        while (i < elen && ebuf[i] != '\n') {
            char ch = ebuf[i++];
            if (ch == '\t') ch = ' ';
            if (ch < ' ' || ch >= 0x7F) ch = ' ';
            if (n < cols && n < (int)sizeof(tmp) - 1) tmp[n++] = ch;
        }
        if (i < elen && ebuf[i] == '\n') i++;       /* consume the newline */
        tmp[n] = '\0';
        if (n) fb_text(x, y + r * LN_H, tmp, COL_TEXT, 1);
        if (i >= elen && r < rows) { /* allow the final (possibly empty) line */ }
    }

    /* the caret */
    if (crow >= escroll && crow < escroll + rows) {
        int cc = ccol > cols ? cols : ccol;
        fb_rect(x + cc * CH_W, y + (crow - escroll) * LN_H, 1, CH_W + 1, COL_CARET);
    }
}

int editor_copy(char *out, int max)
{
    int n = elen < max - 1 ? elen : max - 1;
    if (n < 0) n = 0;
    memcpy(out, ebuf, (size_t)n);
    out[n] = '\0';
    return n;
}

void editor_paste(const char *s)
{
    for (; *s; s++) editor_key(*s);     /* reuse insert (handles '\n', etc.) */
}

int editor_save(void)
{
    if (!enode) return -1;
    if (fs_set_content(enode, ebuf, (uint64_t)elen) < 0) return -1;
    edirty = 0;
    ever++;
    return 0;
}
