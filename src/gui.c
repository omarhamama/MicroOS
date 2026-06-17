/*
 * gui.c — the MicroOS desktop, with a real window manager.
 *
 * A kernel task repaints a double-buffered framebuffer on change. The
 * LOOK comes from two techniques (see fb.c): alpha blending (frosted
 * menu bar, dock, soft shadows) and rounded geometry. The BEHAVIOR
 * comes from a tiny window manager built right here:
 *
 *   - a table of windows, each with a STATE (normal / minimized /
 *     closed) and a draw_body() callback for its content
 *   - a Z-ORDER list: windows paint back-to-front, so the focused one
 *     is on top; clicking any window raises it to the front
 *   - the three traffic lights WORK: red closes, yellow minimizes to
 *     the dock, green zooms (toggles near-fullscreen)
 *   - the dock restores a minimized or closed window and focuses it
 *   - clicks respect occlusion: only the topmost window under the
 *     cursor receives a click (so a button hidden behind another
 *     window can't be pressed through it)
 *
 * No toolkit, no compositor process — every behavior is a short
 * function you can read. This is, in miniature, what a window server
 * (Quartz, X11, Wayland) does: own the screen, stack the windows,
 * route the input.
 */

#include "gui.h"
#include "fb.h"
#include "text.h"
#include "virtio_input.h"
#include "virtio_snd.h"
#include "net.h"
#include "task.h"
#include "timer.h"
#include "mem.h"
#include "mfs.h"
#include "smp.h"
#include "rtc.h"
#include "fs.h"
#include "browser.h"
#include "player.h"
#include "editor.h"
#include "imgview.h"
#include "power.h"
#include "board.h"
#include "fonts.h"
#include "lib.h"
#include "kprintf.h"

static const char *weekday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *month_nm[] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---- palette -------------------------------------------------------- */
#define WALL_A         0x3a6ea5
#define WALL_B         0x7a4a8c
#define COL_BAR_TEXT   0x1d1d1f
#define COL_WIN_BODY   0xf5f5f7
#define COL_TITLE_ACT  0x4a4a4c       /* focused title text */
#define COL_TITLE_INACT 0xb0b0b4      /* unfocused title text */
#define COL_WIN_TEXT   0x1d1d1f
#define COL_MUTED      0x86868b
#define COL_ACCENT     0x007aff
#define COL_TL_RED     0xff5f57
#define COL_TL_YELLOW  0xfebc2e
#define COL_TL_GREEN   0x28c840
#define COL_TL_DIM     0x4a4a4c        /* lights when window unfocused */
#define COL_TERM_BG    0x1c1c1f
#define COL_TERM_BAR   0x2c2c30
#define COL_TERM_TEXT  0xd8d8dc
#define COL_TERM_GREEN 0x7ce38b

#define TITLE_H 28

static volatile int active;
static volatile uint64_t frames;
int gui_active(void)       { return active; }
uint64_t gui_frames(void)  { return frames; }

/* ---- the window table ----------------------------------------------- */

enum win_state { WS_NORMAL, WS_MINIMIZED, WS_CLOSED };

struct window {
    int x, y, w, h;                 /* current geometry */
    int sx, sy, sw, sh;             /* saved geometry while zoomed */
    int zoomed;
    enum win_state state;
    int dark;                       /* Terminal.app styling */
    const char *title;
    const char *app;                /* short app name for the menu bar */
    void (*body)(struct window *);  /* draws content inside the frame */
    int glyph;                      /* dock icon to draw */
};

#define MAXWIN 12
static struct window wins[MAXWIN];
static int nwin;
static int zorder[MAXWIN];          /* zorder[0]=back ... [nwin-1]=front */

static int win_add(const char *title, int x, int y, int w, int h,
                   int dark, void (*body)(struct window *), int glyph)
{
    int id = nwin++;
    struct window *win = &wins[id];
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->state = WS_NORMAL;
    win->dark = dark;
    win->title = title;
    win->app = title;               /* default app name = window title */
    win->body = body;
    win->glyph = glyph;
    zorder[id] = id;                /* newest on top */
    return id;
}

static void raise_win(int id)       /* move id to the front of the z-order */
{
    int i;
    for (i = 0; i < nwin; i++)
        if (zorder[i] == id)
            break;
    for (; i < nwin - 1; i++)
        zorder[i] = zorder[i + 1];
    zorder[nwin - 1] = id;
}

static int focused_win(void)        /* topmost NORMAL window, or -1 */
{
    for (int i = nwin - 1; i >= 0; i--)
        if (wins[zorder[i]].state == WS_NORMAL)
            return zorder[i];
    return -1;
}

/* ---- the console mirror (the Terminal window's content) ------------- */

#define CON_COLS 64
#define CON_ROWS 62

static char con[CON_ROWS][CON_COLS];
static volatile int con_row, con_col;
static volatile uint32_t con_version;

static void con_scroll(void)
{
    memcpy(con, con[1], (CON_ROWS - 1) * CON_COLS);
    memset(con[CON_ROWS - 1], 0, CON_COLS);
}

void gui_console_feed(char c)
{
    if ((unsigned char)c >= 0x80) {
        if (((unsigned char)c & 0xC0) != 0xC0)
            return;
        c = '-';
    }
    if (c == '\r')
        return;
    if (c == '\n' || con_col >= CON_COLS) {
        con_col = 0;
        if (++con_row >= CON_ROWS) {
            con_row = CON_ROWS - 1;
            con_scroll();
        }
        if (c == '\n') { con_version++; return; }
    }
    if (c == '\b') {
        if (con_col > 0)
            con[con_row][--con_col] = 0;
        con_version++;
        return;
    }
    if (c >= ' ' && c < 0x7F)
        con[con_row][con_col++] = c;
    con_version++;
}

/* ---- per-frame input state ------------------------------------------ */

static int frame_mx, frame_my, frame_click;
static int click_target = -1;       /* window id that owns this click (occlusion) */
static int cur_body;                /* id of the window whose body is drawing */
static int browser_win = -1;        /* the Browser window (for key routing) */
static int editor_win  = -1;        /* the TextEdit window (for key routing) */
static int about_win   = -1;        /* the "About MicroOS" window */
static int imgview_win = -1;        /* the Preview (image viewer) window */

/* Route an on-screen keystroke. If the Browser's address bar is focused
 * it eats the key; if the TextEdit window is focused it gets the key;
 * otherwise the key falls through to the shell. Called from the keyboard
 * driver. */
int gui_handle_key(char c)
{
    if (browser_win >= 0 && focused_win() == browser_win && browser_addr_editing())
        return browser_addr_key(c);
    if (editor_win >= 0 && focused_win() == editor_win) {
        editor_key(c);
        return 1;
    }
    return 0;
}

/* Arrow keys move the TextEdit caret when it's the front window
 * (dir: 0=left 1=right 2=up 3=down). Returns 1 if consumed. */
int gui_arrow(int dir)
{
    if (editor_win >= 0 && focused_win() == editor_win) {
        editor_arrow(dir);
        return 1;
    }
    if (browser_win >= 0 && focused_win() == browser_win && browser_addr_editing()) {
        browser_addr_arrow(dir);    /* move the URL caret (and eat ↑/↓ too) */
        return 1;
    }
    return 0;
}

/* Arrow/Page keys scroll the Browser when it's the front window. Returns
 * 1 if consumed. (The periodic repaint shows the new position.) */
int gui_scroll(int amount)
{
    if (browser_win >= 0 && focused_win() == browser_win && !browser_addr_editing()) {
        browser_scroll(amount);
        return 1;
    }
    return 0;
}

/* A button fires only if its OWN window received the click — so a
 * button hidden behind another window can't be pressed through it. */
static int ui_button(int x, int y, const char *label)
{
    int w = fb_text_width(label, 1) + 24;
    int h = 24;
    int hover = frame_mx >= x && frame_mx < x + w &&
                frame_my >= y && frame_my < y + h;
    fb_rounded(x, y + 1, w, h, 7, 0xc9c9ce);
    fb_rounded(x, y, w, h, 7, hover ? 0x2989ff : COL_ACCENT);
    fb_text(x + 12, y + 8, label, 0xffffff, 1);
    return hover && frame_click && cur_body == click_target;
}

static void beep_task(uint64_t a)    { (void)a; vsnd_beep(660, 150); }
static void ping_task(uint64_t a)    { (void)a; net_ping(1); }
static void counter_task(uint64_t a)
{
    (void)a;
    for (;;) { current->scratch++; task_sleep(TIMER_HZ / 10); }
}

/* ---- window bodies -------------------------------------------------- */

static void draw_progress(int x, int y, int w, uint64_t num, uint64_t den,
                          uint32_t color)
{
    fb_rounded(x, y, w, 8, 4, 0xdcdce0);
    if (den && num) {
        int fw = (int)((uint64_t)w * num / den);
        fb_rounded(x, y, fw > 8 ? fw : 8, 8, 4, color);
    }
}

static void body_monitor(struct window *win)
{
    static const char *st[] = { "?", "ready", "run", "sleep", "block", "zombie" };
    char buf[64];
    int x = win->x, w = win->w, ty = win->y + TITLE_H + 14;

    fb_text(x + 16, ty, "TASKS", COL_ACCENT, 1); ty += 14;
    struct task_info info[MAX_TASKS];
    int n = task_snapshot(info, MAX_TASKS);
    for (int i = 0; i < n; i++) {
        ksprintf(buf, sizeof(buf), "%d  %s  %s", info[i].id,
                 st[info[i].state >= 0 && info[i].state <= 5 ? info[i].state : 0],
                 info[i].name);
        fb_text(x + 16, ty, buf, COL_WIN_TEXT, 1);
        ksprintf(buf, sizeof(buf), "%lu ticks", info[i].ticks);
        fb_text(x + 196, ty, buf, COL_MUTED, 1);
        ty += 12;
    }
    ksprintf(buf, sizeof(buf), "idle %lu", task_idle_ticks());
    fb_text(x + 16, ty, buf, COL_MUTED, 1);
    if (smp_core1_online()) {
        ksprintf(buf, sizeof(buf), "core 1: %lu", smp_core1_count());
        fb_text(x + 196, ty, buf, COL_MUTED, 1);
    }
    ty += 22;

    uint64_t claimed, total, live;
    mem_usage(&claimed, &total, &live);
    fb_text(x + 16, ty, "MEMORY", COL_ACCENT, 1); ty += 14;
    ksprintf(buf, sizeof(buf), "%lu KiB of %lu MiB   %lu live",
             claimed >> 10, total >> 20, live);
    fb_text(x + 16, ty, buf, COL_WIN_TEXT, 1); ty += 14;
    draw_progress(x + 16, ty, w - 32, claimed, total, COL_ACCENT); ty += 22;

    uint32_t bu, bt;
    mfs_block_usage(&bu, &bt);
    fb_text(x + 16, ty, "DISK (MicroFS v3)", COL_ACCENT, 1); ty += 14;
    if (bt) {
        ksprintf(buf, sizeof(buf), "%u of %u blocks   %u KiB free",
                 bu, bt, (bt - bu) / 2);
        fb_text(x + 16, ty, buf, COL_WIN_TEXT, 1); ty += 14;
        draw_progress(x + 16, ty, w - 32, bu, bt, 0xff9500); ty += 22;
    }

    if (vinput_present()) {
        fb_text(x + 16, ty, "ACTIONS", COL_ACCENT, 1); ty += 16;
        if (ui_button(x + 16, ty, "beep"))
            task_create("beep", beep_task, 0);
        if (ui_button(x + 82, ty, "spawn"))
            task_create("counter", counter_task, 0);
        if (ui_button(x + 158, ty, "ping"))
            task_create("ping", ping_task, 0);
    }
}

static void body_console(struct window *win)
{
    int x = win->x, y = win->y;
    /* clip rows to the window height in case it's been zoomed/resized */
    int maxrows = (win->h - TITLE_H - 16) / 9;
    if (maxrows > CON_ROWS) maxrows = CON_ROWS;
    int first = con_row >= maxrows ? con_row - maxrows + 1 : 0;
    for (int r = 0; r + first < CON_ROWS && r < maxrows; r++) {
        char line[CON_COLS + 1];
        memcpy(line, con[r + first], CON_COLS);
        line[CON_COLS] = '\0';
        if (line[0])
            fb_text_utf8(x + 14, y + TITLE_H + 10 + r * 9, line,
                    line[0] == '[' ? COL_TERM_GREEN : COL_TERM_TEXT, 1);
    }
    int cr = con_row - first;
    if (cr >= 0 && cr < maxrows)
        fb_rect(x + 14 + con_col * 8, y + TITLE_H + 10 + cr * 9, 8, 8,
                COL_TERM_GREEN);
}

static void body_help(struct window *win)
{
    static const char *lines[] = {
        "Window manager:",
        "  drag a title bar to move a window",
        "  RED light    = close",
        "  YELLOW light = minimize to the dock",
        "  GREEN light  = zoom (toggle big)",
        "  click a window to bring it to the front",
        "  click a dock icon to reopen/raise it",
        "",
        "Type into the Terminal: ls, lisp, ps, help",
        0,
    };
    int x = win->x + 16, y = win->y + TITLE_H + 16;
    for (int i = 0; lines[i]; i++, y += 13)
        fb_text(x, y, lines[i], i == 0 ? COL_ACCENT : COL_WIN_TEXT, 1);
}

/* ---- the file manager — Finder-style ------------------------------- */
/*
 * Walks the SAME fs_node tree the shell's ls/cd use (fs.c). Finder's
 * interaction model: single-click SELECTS an item; double-click a
 * folder OPENS it; a toolbar (New / Copy / Cut / Paste / Delete) acts
 * on the selection. Copy/Cut load a "clipboard"; Paste duplicates into
 * the current folder (and Cut deletes the original — move = copy +
 * delete, so the disk write-through hooks stay consistent). Delete is
 * recursive. The window just renders the live tree each frame.
 */

static struct fs_node *fm_cwd;          /* folder being shown */
static struct fs_node *fm_preview;      /* selected file (shown in preview) */
static struct fs_node *fm_sel;          /* selected entry (file OR folder) */
static struct fs_node *fm_clip;         /* clipboard node, or 0 */
static int             fm_clip_move;    /* 1 = cut (move), 0 = copy */
static struct fs_node *fm_lastclick;    /* for double-click detection */
static uint64_t        fm_lastclick_t;

#define ROW_H 16

/* a clickable list row with a little folder/file icon; returns 1 on click */
static int ui_row(int x, int y, int w, const char *label, int is_dir, int sel)
{
    int hover = frame_mx >= x && frame_mx < x + w &&
                frame_my >= y && frame_my < y + ROW_H;
    if (sel)
        fb_rect(x, y - 1, w, ROW_H, 0xd6e6ff);          /* selected file */
    else if (hover)
        fb_rect(x, y - 1, w, ROW_H, 0xe8e8ec);          /* hover highlight */

    if (is_dir) {                                       /* folder glyph */
        fb_rect(x + 2, y + 4, 12, 8, 0xe0a83a);
        fb_rect(x + 2, y + 2, 6, 3, 0xe0a83a);
    } else {                                            /* page glyph */
        fb_rect(x + 3, y + 1, 9, 12, 0xffffff);
        fb_rect(x + 3, y + 1, 9, 12, 0xffffff);
        fb_rect(x + 4, y + 4, 7, 1, 0xb0b0b4);
        fb_rect(x + 4, y + 7, 7, 1, 0xb0b0b4);
    }
    fb_text_utf8(x + 20, y, label, COL_WIN_TEXT, 1);   /* Arabic file names render */
    return hover && frame_click && cur_body == click_target;
}

/* paste the clipboard into the current folder (Cut also deletes src) */
static void fm_paste(void)
{
    if (!fm_clip)
        return;
    /* never paste a folder into itself or one of its own children */
    if (fm_clip->type == FS_DIR && fs_is_descendant(fm_clip, fm_cwd))
        return;
    char name[FS_NAME_MAX];
    fs_make_unique(fm_cwd, fm_clip->name, name, sizeof(name));
    struct fs_node *n = fs_copy(fm_clip, fm_cwd, name);
    if (n && fm_clip_move)
        fs_delete_recursive(fm_clip);   /* move = copy + delete original */
    fm_clip = 0;
    fm_sel = n;
    fm_preview = (n && n->type == FS_FILE) ? n : 0;
}

static void body_files(struct window *win)
{
    char buf[80];
    int x = win->x + 14, w = win->w - 28;
    int y = win->y + TITLE_H + 12;
    int bottom = win->y + win->h - 12;

    if (!fm_cwd)
        fm_cwd = fs_root;

    /* breadcrumb */
    fs_path(fm_cwd, buf, sizeof(buf));
    fb_text(x, y, buf, COL_ACCENT, 1);
    y += 16;

    /* toolbar — each button acts on the current selection / clipboard */
    int bx = x;
    if (ui_button(bx, y, "New")) {
        char nm[FS_NAME_MAX];
        fs_make_unique(fm_cwd, "folder", nm, sizeof(nm));
        fs_create(fm_cwd, nm, FS_DIR);
        return;
    }
    bx += fb_text_width("New", 1) + 30;
    if (ui_button(bx, y, "Copy") && fm_sel) { fm_clip = fm_sel; fm_clip_move = 0; }
    bx += fb_text_width("Copy", 1) + 30;
    if (ui_button(bx, y, "Cut") && fm_sel)  { fm_clip = fm_sel; fm_clip_move = 1; }
    bx += fb_text_width("Cut", 1) + 30;
    if (ui_button(bx, y, "Paste")) { fm_paste(); return; }
    bx += fb_text_width("Paste", 1) + 30;
    if (ui_button(bx, y, "Delete")) {
        if (fm_sel && fm_sel->parent) {
            if (fm_clip == fm_sel) fm_clip = 0;
            fs_delete_recursive(fm_sel);
            fm_sel = 0;
            fm_preview = 0;
        }
        return;
    }
    y += 30;

    /* status line: clipboard, or selection, or a hint */
    if (fm_clip) {
        ksprintf(buf, sizeof(buf), "clipboard: %s '%s' — Paste to place here",
                 fm_clip_move ? "move" : "copy", fm_clip->name);
        fb_text(x, y, buf, COL_MUTED, 1);
    } else if (fm_sel) {
        ksprintf(buf, sizeof(buf), "selected: %s", fm_sel->name);
        fb_text(x, y, buf, COL_MUTED, 1);
    } else {
        fb_text(x, y, "double-click a folder to open, a file to edit",
                COL_MUTED, 1);
    }
    y += 18;

    int list_bottom = fm_preview ? bottom - 120 : bottom;

    if (fm_cwd->parent) {               /* ".." to go up, except at root */
        if (ui_row(x, y, w, "..", 1, 0)) {
            fm_cwd = fm_cwd->parent;
            fm_sel = 0;
            fm_preview = 0;
            return;
        }
        y += ROW_H;
    }

    int sel_ok = 0;
    for (struct fs_node *c = fm_cwd->children; c; c = c->next) {
        if (y + ROW_H > list_bottom) {
            fb_text(x, y, "...", COL_MUTED, 1);
            break;
        }
        if (c == fm_sel)
            sel_ok = 1;
        if (c->type == FS_DIR)
            ksprintf(buf, sizeof(buf), "%s/", c->name);
        else
            ksprintf(buf, sizeof(buf), "%s   (%lu bytes)", c->name, c->size);

        if (ui_row(x, y, w, buf, c->type == FS_DIR, c == fm_sel)) {
            uint64_t now = timer_ticks();
            int dbl = (c == fm_lastclick) && (now - fm_lastclick_t < TIMER_HZ);
            fm_lastclick = c;
            fm_lastclick_t = now;
            fm_sel = c;
            sel_ok = 1;
            if (c->type == FS_DIR && dbl) {     /* double-click a folder: open it */
                fm_cwd = c;
                fm_sel = 0;
                fm_preview = 0;
                fm_lastclick = 0;
                return;
            }
            if (c->type == FS_FILE && dbl) {    /* open it: image -> Preview */
                if (img_is_image(c->name) && imgview_win >= 0) {
                    imgview_open(c);
                    wins[imgview_win].state = WS_NORMAL;
                    raise_win(imgview_win);
                } else if (editor_win >= 0) {   /* anything else -> TextEdit */
                    editor_open(c);
                    wins[editor_win].state = WS_NORMAL;
                    raise_win(editor_win);
                }
                fm_lastclick = 0;
                return;
            }
            fm_preview = (c->type == FS_FILE) ? c : 0;
        }
        y += ROW_H;
    }
    if (!fm_cwd->children)
        fb_text(x, y, "(empty folder)", COL_MUTED, 1);

    if (!sel_ok) {                      /* selection left this folder */
        fm_sel = 0;
        fm_preview = 0;
    }

    /* preview pane: the first lines of the selected file's bytes */
    if (fm_preview) {
        int py = bottom - 112;
        fb_rect(x, py, w, 2, 0xdcdce0);
        py += 6;
        ksprintf(buf, sizeof(buf), "%s — %lu bytes", fm_preview->name,
                 fm_preview->size);
        fb_text(x, py, buf, COL_ACCENT, 1);
        py += 14;
        /* an image? show a thumbnail (double-click to open it full size) */
        if (imgview_thumb(fm_preview, x, py, w, bottom - py - 2))
            return;
        int col = 0;
        char line[80];
        int li = 0;
        for (uint64_t i = 0; i < fm_preview->size && py < bottom; i++) {
            char ch = fm_preview->data[i];
            if (ch == '\n' || col >= 60) {
                line[li] = 0;
                fb_text(x, py, line, COL_WIN_TEXT, 1);
                py += 11;
                li = 0;
                col = 0;
                if (ch != '\n')
                    i--;                /* wrapped, re-handle this char */
                continue;
            }
            if (ch >= ' ' && ch < 0x7F && li < 78) {
                line[li++] = ch;
                col++;
            }
        }
        if (li && py < bottom) {
            line[li] = 0;
            fb_text(x, py, line, COL_WIN_TEXT, 1);
        }
    }
}

/* ---- the font manager window ---------------------------------------- */

static void body_fonts(struct window *win)
{
    int x = win->x + 16, w = win->w - 32;
    int y = win->y + TITLE_H + 14;

    fb_text(x, y, "SYSTEM FONT - click one to use it everywhere", COL_ACCENT, 1);
    y += 22;

    for (int i = 0; i < fonts_count(); i++) {
        int sel = (i == fonts_active());
        int hover = frame_mx >= x - 4 && frame_mx < x + w + 4 &&
                    frame_my >= y - 4 && frame_my < y + 28;
        if (sel)        fb_rect(x - 4, y - 4, w + 8, 30, 0xd6e6ff);
        else if (hover) fb_rect(x - 4, y - 4, w + 8, 30, 0xeeeef2);

        fb_text(x, y + 4, sel ? "*" : " ", COL_ACCENT, 1);
        fb_text(x + 16, y + 4, fonts_name(i), COL_WIN_TEXT, 1);
        /* preview each font IN its own face */
        fb_text_with(x + 110, y, "Aa Bb 0123", COL_WIN_TEXT, 2, fonts_glyphs(i));

        if (hover && frame_click && cur_body == click_target)
            fonts_set(i);
        y += 34;
    }

    y += 8;
    fb_text(x, y, "GET A FONT FROM THE WEB", COL_ACCENT, 1); y += 16;
    fb_text(x, y, "Terminal:  fontget <url>   (a .mf8 bitmap, plain HTTP)",
            COL_WIN_TEXT, 1); y += 16;
    fb_text(x, y, "Google Fonts can't be used: they're HTTPS-only and ship",
            COL_MUTED, 1); y += 12;
    fb_text(x, y, "vector TTF, which need TLS + a rasterizer we don't have.",
            COL_MUTED, 1);
}

/* ---- the web browser window ----------------------------------------- */

/* A little padlock glyph for the address bar: a rounded body with a
 * shackle ring on top. Green = certificate verified, red = TLS failed,
 * grey = plain HTTP (no security). */
static void draw_lock(int x, int y, uint32_t c)
{
    fb_rect(x + 3, y, 6, 2, c);             /* shackle top */
    fb_rect(x + 3, y, 2, 6, c);             /* shackle sides */
    fb_rect(x + 7, y, 2, 6, c);
    fb_rounded(x, y + 5, 12, 9, 2, c);      /* body */
    fb_rect(x + 5, y + 8, 2, 3, 0xffffff);  /* keyhole */
}

static void body_browser(struct window *win)
{
    int bx = win->x + 14;
    int aby = win->y + TITLE_H + 10;

    /* toolbar: Back, Home, lock, then a (read-only) address field */
    if (ui_button(bx, aby, "< Back"))
        browser_back();
    bx += fb_text_width("< Back", 1) + 30;
    if (ui_button(bx, aby, "Home"))
        browser_navigate("about:home");
    bx += fb_text_width("Home", 1) + 30;

    int sec = browser_security();
    uint32_t lc = sec > 0 ? 0x28c840 : sec < 0 ? 0xff3b30 : 0x86868b;
    draw_lock(bx, aby + 4, lc);
    bx += 20;

    int field = win->x + win->w - 14 - bx;
    int editing = browser_addr_editing();
    /* click inside the address field: first click focuses (select-all);
     * a click while already editing places the caret where you clicked. */
    if (frame_click && cur_body == click_target &&
        frame_mx >= bx && frame_mx < bx + field &&
        frame_my >= aby && frame_my < aby + 24) {
        browser_addr_clickfield(frame_mx - (bx + 8));   /* focus or move caret */
        editing = 1;
    }
    fb_rounded(bx, aby, field, 24, 5, 0xffffff);
    if (editing)                                /* highlight the focused field */
        fb_rect(bx, aby + 22, field, 2, COL_ACCENT);
    const char *atext = browser_addr_text();
    fb_text(bx + 8, aby + 8, atext, editing ? COL_WIN_TEXT : 0x515154, 1);
    if (editing) {                              /* caret at its position in the URL */
        int cxp = bx + 8 + browser_addr_cursor() * 8;   /* 8 px per glyph */
        if (cxp < bx + field - 4) fb_rect(cxp + 1, aby + 6, 1, 12, COL_ACCENT);
    }

    /* a one-line security status strip below the toolbar */
    int py = aby + 30;
    if (sec != 0) {
        char line[128];
        if (sec > 0)
            ksprintf(line, sizeof line, "Secure - certificate verified to root: %s",
                     browser_security_text());
        else
            ksprintf(line, sizeof line, "NOT SECURE - %s", browser_security_text());
        fb_text(win->x + 14, py, line, sec > 0 ? 0x1a8a2e : 0xff3b30, 1);
        py += 16;
    }

    /* the page itself */
    py += 4;
    int ph = win->y + win->h - py - 10;
    browser_draw(win->x + 8, py, win->w - 16, ph);

    /* a click in the page area may land on a link */
    if (frame_click && cur_body == click_target && frame_my >= py)
        browser_click(frame_mx, frame_my);
}

/* ---- the text editor ------------------------------------------------ */

static void body_editor(struct window *win)
{
    int x = win->x + 14, y = win->y + TITLE_H + 10;

    /* toolbar: Save + the filename and a dirty marker */
    if (ui_button(x, y, "Save"))
        editor_save();
    int hx = x + fb_text_width("Save", 1) + 36;
    char hdr[64];
    ksprintf(hdr, sizeof hdr, "%s%s", editor_filename(),
             editor_dirty() ? "  (modified)" : "");
    fb_text(hx, y + 8, hdr, editor_dirty() ? 0xc04000 : COL_MUTED, 1);

    int focused = (focused_win() == cur_body);
    const char *hint = focused ? "" : "click here to type";
    if (hint[0])
        fb_text(win->x + win->w - 14 - fb_text_width(hint, 1), y + 8, hint, COL_MUTED, 1);
    y += 30;

    /* the text area: a white page with the buffer + caret */
    int tx = win->x + 12, ty = y;
    int tw = win->w - 24, th = win->y + win->h - ty - 12;
    fb_rect(tx, ty, tw, th, 0xffffff);
    fb_rect(tx, ty, tw, 1, 0xdcdce0);

    /* a click in the page places the caret (and focuses via raise) */
    if (frame_click && cur_body == click_target &&
        frame_mx >= tx && frame_mx < tx + tw && frame_my >= ty && frame_my < ty + th)
        editor_click(frame_mx - (tx + 6), frame_my - (ty + 6));

    editor_draw(tx + 6, ty + 6, tw - 12, th - 12);
}

/* ---- the image viewer (Preview) ------------------------------------- */

static void body_imgview(struct window *win)
{
    int x = win->x + 12, y = win->y + TITLE_H + 10;
    char hdr[48];
    ksprintf(hdr, sizeof hdr, "%s", imgview_name()[0] ? imgview_name()
                                                       : "Preview");
    fb_text(x, y + 4, hdr, COL_WIN_TEXT, 1);
    y += 24;
    imgview_draw(x, y, win->w - 24, win->y + win->h - y - 12);
}

/* ---- the music player (Winamp-style) -------------------------------- */

/* Transport-button symbols, drawn from primitives (no icon font). */
enum { SYM_PREV, SYM_PLAY, SYM_PAUSE, SYM_STOP, SYM_NEXT };

static void tri_right(int x, int y, int sz, uint32_t c)   /* ▶ */
{
    for (int col = 0; col < sz; col++) {
        int hh = (sz / 2) * (sz - 1 - col) / (sz - 1);
        fb_rect(x + col, y + sz / 2 - hh, 1, hh * 2 + 1, c);
    }
}
static void tri_left(int x, int y, int sz, uint32_t c)    /* ◀ */
{
    for (int col = 0; col < sz; col++) {
        int hh = (sz / 2) * col / (sz - 1);
        fb_rect(x + col, y + sz / 2 - hh, 1, hh * 2 + 1, c);
    }
}

/* A small dark transport button; returns 1 if clicked this frame. */
static int xport_btn(int x, int y, int sz, int sym)
{
    int hover = frame_mx >= x && frame_mx < x + sz &&
                frame_my >= y && frame_my < y + sz;
    fb_rounded(x, y, sz, sz, 5, hover ? 0x3a3a40 : 0x2a2a2e);
    uint32_t c = 0xe6e6ea;
    int cx = x + sz / 2, cy = y + sz / 2, g = sz / 3;
    switch (sym) {
    case SYM_PREV:
        fb_rect(cx - g - 2, cy - g, 2, g * 2, c);
        tri_left(cx - g, cy - g, g * 2, c);
        break;
    case SYM_PLAY:  tri_right(cx - g + 1, cy - g, g * 2, c); break;
    case SYM_PAUSE:
        fb_rect(cx - g + 1, cy - g, 3, g * 2, c);
        fb_rect(cx + g - 4, cy - g, 3, g * 2, c);
        break;
    case SYM_STOP:  fb_rect(cx - g, cy - g, g * 2, g * 2, c); break;
    case SYM_NEXT:
        tri_right(cx - g, cy - g, g * 2, c);
        fb_rect(cx + g, cy - g, 2, g * 2, c);
        break;
    }
    return hover && frame_click && cur_body == click_target;
}

/* A horizontal slider. `frac` is the fill (0..1000). If the user clicks
 * the track this frame, returns the clicked fraction (0..1000), else -1. */
static int hslider(int x, int y, int w, int frac, uint32_t fill)
{
    fb_rounded(x, y, w, 5, 2, 0x3a3a40);
    int fw = frac * w / 1000;
    if (fw > 0) fb_rounded(x, y, fw < 4 ? 4 : fw, 5, 2, fill);
    fb_rect(x + fw - 1, y - 3, 3, 11, 0xe6e6ea);            /* the knob */
    if (frame_click && cur_body == click_target &&
        frame_mx >= x - 2 && frame_mx <= x + w + 2 &&
        frame_my >= y - 7 && frame_my <= y + 12) {
        int f = (frame_mx - x) * 1000 / (w > 0 ? w : 1);
        return f < 0 ? 0 : f > 1000 ? 1000 : f;
    }
    return -1;
}

static void fmt_time(char *buf, int sz, uint32_t frames)
{
    uint32_t s = frames / 44100;
    ksprintf(buf, sz, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

static void body_player(struct window *win)
{
    int x = win->x, y = win->y, w = win->w;
    int px = x + 12, pw = w - 24;
    int top = y + TITLE_H + 12;
    char buf[64];

    int st = player_get_state();
    uint32_t pos = player_pos_frames(), total = player_total_frames();

    /* ---- LCD: big time + state + format ---- */
    int lcdh = 40;
    fb_rounded(px, top, pw, lcdh, 6, 0x0a140c);
    fmt_time(buf, sizeof buf, pos);
    fb_text(px + 10, top + 9, buf, 0x35ff6a, 3);
    /* state word + sample format, right-aligned */
    const char *sw = st == PS_PLAYING ? "PLAY" : st == PS_PAUSED ? "PAUSE" : "STOP";
    fb_text(px + pw - 10 - fb_text_width(sw, 1), top + 6, sw, 0x35ff6a, 1);
    fb_text(px + pw - 10 - fb_text_width("44kHz stereo", 1), top + 22,
            "44kHz stereo", 0x1f8a44, 1);

    /* ---- spectrum analyser ---- */
    int spy = top + lcdh + 6, sph = 46;
    fb_rounded(px, spy, pw, sph, 6, 0x0a0f0a);
    int bvals[PLAYER_NBANDS];
    int nb = player_spectrum(bvals, PLAYER_NBANDS);
    int innerw = pw - 12, bw = innerw / nb;
    int base = spy + sph - 5;
    for (int i = 0; i < nb; i++) {
        int bx = px + 6 + i * bw;
        int bh = bvals[i] * (sph - 10) / PLAYER_BAR_MAX;
        for (int yy = 0; yy < bh; yy++) {
            /* green at the bottom, through yellow, to red at the peak */
            uint32_t c = yy > (sph - 10) * 3 / 4 ? 0xff3b30
                       : yy > (sph - 10) / 2     ? 0xffd60a
                       : 0x30d158;
            fb_rect(bx, base - yy, bw - 2, 1, c);
        }
    }

    /* ---- scrolling title (a fixed-width ticker, never overflows) ---- */
    int ty = spy + sph + 8;
    fb_rounded(px, ty, pw, 16, 4, 0x0a140c);
    {
        const char *title = player_title();
        int len = (int)strlen(title);
        int vis = (pw - 16) / 8;
        char line[64];
        if (vis > (int)sizeof(line) - 1) vis = sizeof(line) - 1;
        if (len <= vis) {
            fb_text(px + 8, ty + 4, title, 0x35ff6a, 1);
        } else {
            int span = len + 4;                 /* title + a 4-space gap */
            int start = (int)((timer_ticks() / 6) % span);
            for (int i = 0; i < vis; i++) {
                int j = (start + i) % span;
                line[i] = j < len ? title[j] : ' ';
            }
            line[vis] = '\0';
            fb_text(px + 8, ty + 4, line, 0x35ff6a, 1);
        }
    }

    /* ---- a status note (e.g. "MP3 decoder not built yet") ---- */
    int row = ty + 22;
    const char *status = player_status();
    if (status[0]) {
        fb_text(px, row, status, 0xff9f0a, 1);
    }
    row += 14;

    /* ---- seek bar ---- */
    int frac = total ? (int)((uint64_t)pos * 1000 / total) : 0;
    int sk = hslider(px, row + 4, pw, frac, 0x35c95a);
    if (sk >= 0) player_seek_frac(sk);
    fmt_time(buf, sizeof buf, total);
    fb_text(px + pw - fb_text_width(buf, 1), row + 12, buf, COL_MUTED, 1);
    row += 22;

    /* ---- transport buttons ---- */
    int bsz = 30, gap = 8;
    int bx = px + (pw - (5 * bsz + 4 * gap)) / 2;
    if (xport_btn(bx + 0 * (bsz + gap), row, bsz, SYM_PREV))  player_prev();
    if (xport_btn(bx + 1 * (bsz + gap), row, bsz,
                  st == PS_PLAYING ? SYM_PAUSE : SYM_PLAY))   player_toggle();
    if (xport_btn(bx + 2 * (bsz + gap), row, bsz, SYM_STOP))  player_stop();
    if (xport_btn(bx + 3 * (bsz + gap), row, bsz, SYM_NEXT))  player_next();
    /* (4th slot left as breathing room / symmetry) */
    row += bsz + 12;

    /* ---- volume ---- */
    fb_text(px, row + 2, "VOL", COL_MUTED, 1);
    int vfrac = player_volume() * 1000 / 256;
    int vv = hslider(px + 34, row + 4, pw - 34, vfrac, 0x0a84ff);
    if (vv >= 0) player_set_volume(vv * 256 / 1000);
    row += 20;

    /* ---- playlist ---- */
    fb_rect(px, row, pw, 1, 0x2a2a2e);
    row += 6;
    int n = player_track_count(), curi = player_current_track();
    int maxrows = (y + win->h - 12 - row) / 15;
    for (int i = 0; i < n && i < maxrows; i++) {
        int ry = row + i * 15;
        int hover = frame_mx >= px && frame_mx < px + pw &&
                    frame_my >= ry && frame_my < ry + 15;
        if (i == curi) fb_rounded(px - 2, ry - 1, pw + 4, 15, 3, 0x232a23);
        uint32_t tc = i == curi ? 0x35ff6a : hover ? 0xffffff : 0xb0b0b6;
        ksprintf(buf, sizeof buf, "%d.", i + 1);
        fb_text(px + 2, ry + 3, buf, COL_MUTED, 1);
        char nm[40];
        const char *src = player_track_name(i);
        int k = 0, vis = (pw - 26) / 8;
        if (vis > (int)sizeof(nm) - 1) vis = sizeof(nm) - 1;
        for (; src[k] && k < vis; k++) nm[k] = src[k];
        nm[k] = '\0';
        fb_text_utf8(px + 22, ry + 3, nm, tc, 1);
        if (hover && frame_click && cur_body == click_target)
            player_select(i);
    }
}

/* ---- the About box -------------------------------------------------- */

static void about_center(int cx, int y, const char *s, uint32_t c, int scale)
{
    fb_text(cx - fb_text_width(s, scale) / 2, y, s, c, scale);
}

static void body_about(struct window *win)
{
    int cx = win->x + win->w / 2;
    int y = win->y + TITLE_H + 26;

    /* the MicroOS mark: a crescent moon (same shape as the menu bar) */
    fb_circle(cx, y + 4, 20, 0x1d1d1f);
    fb_circle(cx + 8, y - 4, 16, COL_WIN_BODY);
    y += 40;

    about_center(cx, y, "MicroOS", COL_WIN_TEXT, 2);   y += 26;
    about_center(cx, y, "Version 1.1", COL_MUTED, 1);  y += 22;

    static const char *blurb[] = {
        "A from-scratch ARM64 operating system:",
        "kernel, MMU, SMP, a journaled filesystem,",
        "TCP/TLS networking, a web browser, a",
        "Winamp-style MP3 player and a text editor,",
        "every layer hand-built, running on QEMU.",
    };
    for (int i = 0; i < (int)(sizeof blurb / sizeof blurb[0]); i++) {
        about_center(cx, y, blurb[i], COL_WIN_TEXT, 1);
        y += 14;
    }
    y += 10;

    /* a few live vitals, like "About This Mac" */
    char buf[64];
    uint64_t ms = timer_uptime_ms();
    ksprintf(buf, sizeof buf, "Up %lu:%02lu     %s", ms / 60000, (ms / 1000) % 60,
             smp_core1_online() ? "2 cores" : "1 core");
    about_center(cx, y, buf, COL_MUTED, 1); y += 14;

    uint64_t claimed, total, live;
    mem_usage(&claimed, &total, &live);
    ksprintf(buf, sizeof buf, "%lu KiB used of %lu MiB RAM", claimed >> 10, total >> 20);
    about_center(cx, y, buf, COL_MUTED, 1); y += 20;

    about_center(cx, y, "MicroOS was built by Omar Hamama",   COL_WIN_TEXT, 1); y += 14;
    about_center(cx, y, "as a from-scratch learning project",  COL_MUTED, 1);    y += 14;
    about_center(cx, y, "(with the help of Claude).",          COL_MUTED, 1);
}

/* ---- the menu bar's dropdown menus (, File, Edit, Window) ---------- */
/* Each menu-bar title opens a dropdown drawn on top of the windows;
 * clicks are handled before the window manager sees them so a menu always
 * wins. File/Edit act on the FOCUSED app; Window lists the open windows. */

enum { MENU_NONE, MENU_APPLE, MENU_APP, MENU_FILE, MENU_EDIT, MENU_WINDOW };
static int menu_open;                       /* which dropdown is showing */

/* item action ids */
enum {
    MI_NONE,
    MI_ABOUT, MI_RESTART, MI_SHUTDOWN,      /*  */
    MI_SETTINGS, MI_HIDE, MI_QUIT,          /* App menu */
    MI_SAVE, MI_HOME, MI_CLOSE,             /* File */
    MI_COPY, MI_PASTE,                      /* Edit */
    MI_WIN_BASE = 100                       /* Window: MI_WIN_BASE + window id */
};

struct menu_item { char label[32]; int id; int enabled; int divider; int check; };

static char clip[1024];                     /* tiny system clipboard (Edit) */
static int  clip_len;
static int  fonts_win = -1;                 /* the Fonts window = "Settings" */

#define MENU_TOP 27
#define MENU_IH  22

/* The focused app's name — shown (bold) in the menu bar, like macOS. */
static const char *focused_app_name(void)
{
    int f = focused_win();
    if (f < 0) return "MicroOS";
    return wins[f].app ? wins[f].app : wins[f].title;
}

/* Lay the menu-bar titles out left to right:  AppName File Edit Window.
 * Each slot's text is drawn at x0+4; its dropdown anchors at x0. */
struct mb_slot { int which; const char *label; int x0, x1; };
static int menubar_layout(struct mb_slot *s)
{
    int n = 0;
    s[n++] = (struct mb_slot){ MENU_APPLE, "", 2, 28 };     /* the moon logo */
    int x = 32;
    struct { int which; const char *label; } t[4] = {
        { MENU_APP,    focused_app_name() },
        { MENU_FILE,   "File" },
        { MENU_EDIT,   "Edit" },
        { MENU_WINDOW, "Window" },
    };
    for (int i = 0; i < 4; i++) {
        int tw = fb_text_width(t[i].label, 1) + (t[i].which == MENU_APP ? 1 : 0);
        s[n++] = (struct mb_slot){ t[i].which, t[i].label, x, x + 8 + tw };
        x += 8 + tw + 10;
    }
    return n;
}

static void mi_add(struct menu_item *it, int *n, const char *label,
                   int id, int en, int div, int chk)
{
    ksprintf(it[*n].label, sizeof it[*n].label, "%s", label);
    it[*n].id = id; it[*n].enabled = en; it[*n].divider = div; it[*n].check = chk;
    (*n)++;
}

/* Build the item list for a menu. Returns the count. */
static int menu_build(int which, struct menu_item *it)
{
    int n = 0, f = focused_win();
    const char *app = focused_app_name();
    char buf[32];
    if (which == MENU_APPLE) {
        mi_add(it, &n, "About MicroOS", MI_ABOUT, 1, 1, 0);
        mi_add(it, &n, "Restart",   MI_RESTART, 1, 0, 0);
        mi_add(it, &n, "Shut Down", MI_SHUTDOWN, 1, 0, 0);
    } else if (which == MENU_APP) {
        ksprintf(buf, sizeof buf, "About %s", app);
        mi_add(it, &n, buf, MI_ABOUT, 1, 0, 0);
        mi_add(it, &n, "Settings...", MI_SETTINGS, 1, 1, 0);
        mi_add(it, &n, "Hide", MI_HIDE, f >= 0, 0, 0);
        ksprintf(buf, sizeof buf, "Quit %s", app);
        mi_add(it, &n, buf, MI_QUIT, f >= 0, 0, 0);
    } else if (which == MENU_FILE) {
        if (f == editor_win)       mi_add(it, &n, "Save", MI_SAVE, 1, 0, 0);
        else if (f == browser_win) mi_add(it, &n, "Home", MI_HOME, 1, 0, 0);
        mi_add(it, &n, "Close Window", MI_CLOSE, f >= 0, 0, 0);
    } else if (which == MENU_EDIT) {
        int txt = (f == editor_win) ||
                  (f == browser_win && browser_addr_editing());
        mi_add(it, &n, "Copy",  MI_COPY,  txt, 0, 0);
        mi_add(it, &n, "Paste", MI_PASTE, txt && clip_len > 0, 0, 0);
    } else if (which == MENU_WINDOW) {
        for (int i = 0; i < nwin; i++)
            if (wins[i].state != WS_CLOSED)
                mi_add(it, &n, wins[i].title, MI_WIN_BASE + i, 1, 0, i == f);
        if (n == 0) mi_add(it, &n, "(no open windows)", MI_NONE, 0, 0, 0);
    }
    return n;
}

static int menu_width(const struct menu_item *it, int n)
{
    int w = 150;
    for (int i = 0; i < n; i++) {
        int tw = fb_text_width(it[i].label, 1) + 40;
        if (tw > w) w = tw;
    }
    return w;
}

/* Which open menu's item is under the cursor? -1 = none. */
static int menu_item_at(int anchor, int w, int n, int mx, int my)
{
    if (mx < anchor || mx > anchor + w) return -1;
    for (int i = 0; i < n; i++)
        if (my >= MENU_TOP + 4 + i * MENU_IH && my < MENU_TOP + 4 + (i + 1) * MENU_IH)
            return i;
    return -1;
}

static void do_copy(void)
{
    int f = focused_win();
    if (f == editor_win) {
        clip_len = editor_copy(clip, sizeof clip);
    } else if (f == browser_win && browser_addr_editing()) {
        const char *t = browser_addr_text();
        int i = 0;
        for (; t[i] && i < (int)sizeof(clip) - 1; i++) clip[i] = t[i];
        clip[i] = 0; clip_len = i;
    }
}

static void do_paste(void)
{
    int f = focused_win();
    if (f == editor_win) editor_paste(clip);
    else if (f == browser_win && browser_addr_editing())
        for (int i = 0; i < clip_len; i++) browser_addr_key(clip[i]);
}

static void menu_action(int which, int id)
{
    (void)which;
    int f = focused_win();
    if (id >= MI_WIN_BASE) {                    /* Window: bring a window forward */
        int w = id - MI_WIN_BASE;
        if (w >= 0 && w < nwin) { wins[w].state = WS_NORMAL; raise_win(w); }
        return;
    }
    switch (id) {
    case MI_ABOUT:
        if (about_win >= 0) { wins[about_win].state = WS_NORMAL; raise_win(about_win); }
        break;
    case MI_RESTART:  power_reset();
    case MI_SHUTDOWN: power_off();
    case MI_SETTINGS:
        if (fonts_win >= 0) { wins[fonts_win].state = WS_NORMAL; raise_win(fonts_win); }
        break;
    case MI_SAVE:     editor_save(); break;
    case MI_HOME:     browser_navigate("about:home"); break;
    case MI_HIDE:     if (f >= 0) wins[f].state = WS_MINIMIZED; break;
    case MI_QUIT:
    case MI_CLOSE:    if (f >= 0) wins[f].state = WS_CLOSED; break;
    case MI_COPY:     do_copy(); break;
    case MI_PASTE:    do_paste(); break;
    default: break;
    }
}

/* A click on the bar: toggle a title's menu, fire an item, or close.
 * Returns 1 if consumed (so it must not fall through to a window). */
static int handle_menubar(void)
{
    if (!frame_click) return 0;
    struct mb_slot s[6];
    int ns = menubar_layout(s);
    if (frame_my < 26) {                        /* a click on a menu-bar title? */
        for (int i = 0; i < ns; i++)
            if (frame_mx >= s[i].x0 && frame_mx <= s[i].x1) {
                menu_open = (menu_open == s[i].which) ? MENU_NONE : s[i].which;
                return 1;
            }
    }
    if (menu_open != MENU_NONE) {               /* a click while a menu is open */
        struct menu_item it[16];
        int n = menu_build(menu_open, it), w = menu_width(it, n);
        int anchor = 2;
        for (int i = 0; i < ns; i++) if (s[i].which == menu_open) anchor = s[i].x0;
        int idx = menu_item_at(anchor, w, n, frame_mx, frame_my);
        int which = menu_open;
        menu_open = MENU_NONE;                  /* any click closes the menu */
        if (idx >= 0 && it[idx].enabled) menu_action(which, it[idx].id);
        return 1;
    }
    return 0;
}

static void draw_menus(void)
{
    if (menu_open == MENU_NONE) return;
    struct mb_slot s[6];
    int ns = menubar_layout(s);
    struct menu_item it[16];
    int n = menu_build(menu_open, it), w = menu_width(it, n);
    int anchor = 2;
    for (int i = 0; i < ns; i++) if (s[i].which == menu_open) anchor = s[i].x0;
    int h = n * MENU_IH + 8;
    fb_rounded_blend(anchor, MENU_TOP, w, h + 2, 8, 0x000000, 40);   /* shadow */
    fb_rounded_blend(anchor, MENU_TOP, w, h, 8, 0xf6f6f8, 245);      /* panel  */
    int hov = menu_item_at(anchor, w, n, frame_mx, frame_my);
    for (int i = 0; i < n; i++) {
        int y = MENU_TOP + 4 + i * MENU_IH;
        int on = (i == hov) && it[i].enabled;
        if (on) fb_rounded(anchor + 5, y + 1, w - 10, MENU_IH - 2, 5, COL_ACCENT);
        uint32_t c = !it[i].enabled ? 0xb8b8bc : on ? 0xffffff : COL_BAR_TEXT;
        if (it[i].check)                                    /* focused-window mark */
            fb_circle(anchor + 11, y + 9, 2, c);
        fb_text(anchor + 20, y + 5, it[i].label, c, 1);
        if (it[i].divider)
            fb_rect(anchor + 12, y + MENU_IH - 1, w - 24, 1, 0xd2d2d6);
    }
}

/* ---- window chrome -------------------------------------------------- */

static void draw_window(struct window *win, int is_focused)
{
    int x = win->x, y = win->y, w = win->w, h = win->h;
    uint32_t bar = win->dark ? COL_TERM_BAR : 0xe8e8ec;
    uint32_t body = win->dark ? COL_TERM_BG : COL_WIN_BODY;

    /* soft drop shadow, deeper for the focused window */
    int s = is_focused ? 60 : 30;
    fb_rounded_blend(x - 4, y - 2, w + 8, h + 10, 14, 0x000000, (uint8_t)(s / 3));
    fb_rounded_blend(x - 1, y, w + 2, h + 4, 11, 0x000000, (uint8_t)s);

    fb_rounded(x, y, w, h, 10, body);
    fb_rounded(x, y, w, TITLE_H + 10, 10, bar);
    fb_rect(x, y + TITLE_H, w, 10, body);

    fb_text(x + (w - fb_text_width(win->title, 1)) / 2, y + 10, win->title,
            is_focused ? (win->dark ? COL_TERM_TEXT : COL_TITLE_ACT)
                       : COL_TITLE_INACT, 1);

    /* traffic lights (full color only when focused, like macOS) */
    int cy = y + TITLE_H / 2;
    uint32_t lc[3] = { COL_TL_RED, COL_TL_YELLOW, COL_TL_GREEN };
    for (int i = 0; i < 3; i++) {
        int cx = x + 20 + i * 20;
        fb_circle(cx, cy, 6, is_focused ? lc[i] : COL_TL_DIM);
    }

    win->body(win);
}

/* which traffic light is at (mx,my) in this window's title? -1 if none */
static int light_hit(struct window *win, int mx, int my)
{
    int cy = win->y + TITLE_H / 2;
    for (int i = 0; i < 3; i++) {
        int cx = win->x + 20 + i * 20;
        if (mx >= cx - 9 && mx <= cx + 9 && my >= cy - 9 && my <= cy + 9)
            return i;
    }
    return -1;
}

static void toggle_zoom(struct window *win)
{
    if (win->zoomed) {
        win->x = win->sx; win->y = win->sy;
        win->w = win->sw; win->h = win->sh;
        win->zoomed = 0;
    } else {
        win->sx = win->x; win->sy = win->y;
        win->sw = win->w; win->sh = win->h;
        win->x = 8; win->y = 32;
        win->w = FB_WIDTH - 16;
        win->h = FB_HEIGHT - 32 - 84;       /* clear of menu bar and dock */
        win->zoomed = 1;
    }
}

/* ---- desktop layers ------------------------------------------------- */

static void draw_wallpaper(void)
{
    static uint32_t lut[256];
    if (!lut[255]) {
        for (int i = 0; i < 256; i++) {
            uint32_t r = (((WALL_A >> 16) & 0xFF) * (255 - i) + ((WALL_B >> 16) & 0xFF) * i) / 255;
            uint32_t g = (((WALL_A >> 8) & 0xFF) * (255 - i) + ((WALL_B >> 8) & 0xFF) * i) / 255;
            uint32_t b = ((WALL_A & 0xFF) * (255 - i) + (WALL_B & 0xFF) * i) / 255;
            lut[i] = (r << 16) | (g << 8) | b;
        }
    }
    uint32_t *px = fb_pixels();
    for (int y = 0; y < FB_HEIGHT; y++) {
        uint32_t *row = px + (uint64_t)y * FB_WIDTH;
        for (int x = 0; x < FB_WIDTH; x++)
            row[x] = lut[((x + y) * 255) / (FB_WIDTH + FB_HEIGHT)];
    }
}

static void draw_menu_bar(void)
{
    char buf[64];
    fb_blend_rect(0, 0, FB_WIDTH, 26, 0xf6f6f8, 200);

    struct mb_slot s[6];
    int ns = menubar_layout(s);
    /* highlight whichever title's dropdown is open */
    for (int i = 0; i < ns; i++)
        if (menu_open == s[i].which)
            fb_blend_rect(s[i].x0, 0, s[i].x1 - s[i].x0, 26, COL_ACCENT, 255);

    for (int i = 0; i < ns; i++) {
        int open = (menu_open == s[i].which);
        if (s[i].which == MENU_APPLE) {                     /* the moon logo */
            fb_circle(16, 13, 7, open ? 0xffffff : COL_BAR_TEXT);
            fb_circle(19, 10, 4, open ? COL_ACCENT : 0xf6f6f8);
        } else if (s[i].which == MENU_APP) {                /* app name, bold */
            uint32_t c = open ? 0xffffff : COL_BAR_TEXT;
            fb_text(s[i].x0 + 4, 9, s[i].label, c, 1);
            fb_text(s[i].x0 + 5, 9, s[i].label, c, 1);       /* +1px = faux bold */
        } else {                                            /* File / Edit / Window */
            fb_text(s[i].x0 + 4, 9, s[i].label, open ? 0xffffff : 0x515154, 1);
        }
    }

    /* the clock, top-right (like the real thing) — live from the RTC */
    if (rtc_present()) {
        struct datetime dt;
        rtc_datetime(&dt);
        ksprintf(buf, sizeof(buf), "%s %s %d  %02d:%02d:%02d",
                 weekday[dt.wday], month_nm[dt.month], dt.day,
                 dt.hour, dt.min, dt.sec);
    } else {
        uint64_t ms = timer_uptime_ms();
        ksprintf(buf, sizeof(buf), "up %lu:%02lu", ms / 60000, (ms / 1000) % 60);
    }
    fb_text(FB_WIDTH - 16 - fb_text_width(buf, 1), 9, buf, COL_BAR_TEXT, 1);

    /* core count, just left of the clock */
    const char *cores = smp_core1_online() ? "2 cores" : "1 core";
    fb_text(FB_WIDTH - 16 - fb_text_width(buf, 1) - 20 - fb_text_width(cores, 1),
            9, cores, 0x515154, 1);
}

/* ---- the dock ------------------------------------------------------- */
/* Icons 0..nwin-1 map to windows (click = restore + raise); the rest
 * are app launchers. */

struct dock_item { const char *name; uint32_t color; int win; int launch; };

#define L_NONE 0
#define L_BEEP 1
#define L_PING 2
#define L_SPAWN 3

static struct dock_item dock[14];
static int ndock;

static void draw_dock_glyph(int glyph, int x, int y, int s)
{
    switch (glyph) {
    case 0:                                 /* terminal: >_ */
        fb_text(x + s / 2 - 8, y + s / 2 - 4, ">_", COL_TERM_GREEN, 1);
        break;
    case 1:                                 /* monitor */
        fb_rounded(x + 7, y + 8, s - 14, s - 20, 3, 0xffffff);
        fb_rect(x + s / 2 - 2, y + s - 12, 4, 5, 0xffffff);
        fb_rect(x + s / 2 - 7, y + s - 8, 14, 2, 0xffffff);
        break;
    case 2:                                 /* help: ? */
        fb_text(x + s / 2 - 4, y + s / 2 - 4, "?", 0xffffff, 1);
        break;
    case 3:                                 /* speaker */
        fb_rect(x + 9, y + s / 2 - 4, 6, 8, 0xffffff);
        for (int r = 0; r < 14; r++)
            fb_rect(x + 15, y + s / 2 - r / 2, 5 + r / 2, 1, 0xffffff);
        break;
    case 4: {                               /* globe */
        int c = s / 2;
        fb_circle(x + c, y + c, 11, 0xffffff);
        fb_circle(x + c, y + c, 8, 0x30b0c7);
        fb_rect(x + c - 11, y + c - 1, 22, 2, 0xffffff);
        break;
    }
    case 5:                                 /* plus */
        fb_rect(x + s / 2 - 2, y + 9, 4, s - 18, 0xffffff);
        fb_rect(x + 9, y + s / 2 - 2, s - 18, 4, 0xffffff);
        break;
    case 6:                                 /* folder */
        fb_rect(x + 9, y + 14, s - 18, s - 26, 0xffffff);
        fb_rect(x + 9, y + 11, (s - 18) / 2, 4, 0xffffff);
        break;
    case 7: {                               /* browser: globe + meridian */
        int c = s / 2;
        fb_circle(x + c, y + c, 13, 0xffffff);
        fb_circle(x + c, y + c, 11, 0x2a7de1);
        fb_rect(x + c - 11, y + c - 1, 22, 2, 0xffffff);
        fb_rect(x + c - 1, y + c - 11, 2, 22, 0xffffff);
        break;
    }
    case 8:                                 /* fonts: a big "Aa" */
        fb_text(x + s / 2 - 8, y + s / 2 - 8, "Aa", 0xffffff, 2);
        break;
    case 9: {                               /* music: a beamed note */
        int hx = x + s / 2 - 6, hy = y + s - 16;
        fb_rect(hx + 7, y + 10, 2, s - 26, 0xffffff);   /* stem */
        fb_rect(hx + 7, y + 10, 7, 2, 0xffffff);        /* flag/beam */
        fb_circle(hx + 2, hy, 4, 0xffffff);             /* note head */
        break;
    }
    case 10: {                              /* editor: a page with lines */
        int px = x + 11, py = y + 8, pw = s - 22, ph = s - 16;
        fb_rect(px, py, pw, ph, 0xffffff);
        for (int r = 0; r < 4; r++)
            fb_rect(px + 3, py + 5 + r * 6, pw - 6, 1, 0x8e8e93);
        break;
    }
    case 11: {                              /* preview: a photo (sun + hills) */
        int px = x + 9, py = y + 11, pw = s - 18, ph = s - 22;
        fb_rect(px, py, pw, ph, 0xffffff);
        fb_circle(px + pw - 7, py + 6, 3, 0xffd60a);            /* sun */
        for (int c = 0; c < pw - 4; c++) {                      /* hills */
            int hy = c < pw / 2 ? c : (pw - 4 - c);
            fb_rect(px + 2 + c, py + ph - 3 - hy / 2, 1, hy / 2 + 3, 0x34c759);
        }
        break;
    }
    }
}

static void draw_dock(void)
{
    int icon = 44, gap = 12;
    int dw = ndock * icon + (ndock + 1) * gap;
    int dx = (FB_WIDTH - dw) / 2;
    int dy = FB_HEIGHT - icon - 22;

    fb_rounded_blend(dx, dy - 8, dw, icon + 16, 16, 0xf2f2f6, 90);

    for (int i = 0; i < ndock; i++) {
        int x = dx + gap + i * (icon + gap);
        int hover = frame_mx >= x && frame_mx < x + icon &&
                    frame_my >= dy - 8 && frame_my < dy + icon + 8;
        int s = icon, ix = x, iy = dy;
        if (hover) { s = icon + 6; ix = x - 3; iy = dy - 6; }

        int glyph = dock[i].win >= 0 ? wins[dock[i].win].glyph
                  : dock[i].launch == L_BEEP ? 3
                  : dock[i].launch == L_PING ? 4 : 5;
        uint32_t color = dock[i].win >= 0 && wins[dock[i].win].dark ? 0x2c2c30
                       : dock[i].color;
        fb_rounded(ix, iy, s, s, 11, color);
        draw_dock_glyph(glyph, ix, iy, s);

        /* a dot under windows that are open (normal or minimized) */
        if (dock[i].win >= 0 && wins[dock[i].win].state != WS_CLOSED)
            fb_circle(x + icon / 2, dy + icon + 6, 2, 0x1d1d1f);

        if (hover) {
            int tw = fb_text_width(dock[i].name, 1) + 14;
            fb_rounded_blend(x + icon / 2 - tw / 2, dy - 30, tw, 18, 6, 0x2c2c30, 215);
            fb_text(x + icon / 2 - tw / 2 + 7, dy - 25, dock[i].name, 0xffffff, 1);
        }

        if (hover && frame_click) {
            click_target = -1;          /* the dock ate this click */
            if (dock[i].win >= 0) {
                wins[dock[i].win].state = WS_NORMAL;    /* restore */
                raise_win(dock[i].win);
            } else if (dock[i].launch == L_BEEP)  task_create("beep", beep_task, 0);
            else if (dock[i].launch == L_PING)  task_create("ping", ping_task, 0);
            else if (dock[i].launch == L_SPAWN) task_create("counter", counter_task, 0);
        }
    }
}

/* ---- mouse cursor --------------------------------------------------- */

static const char *cursor_shape[] = {
    "#", "##", "#o#", "#oo#", "#ooo#", "#oooo#", "#ooooo#", "#oooooo#",
    "#ooooooo#", "#oooooooo#", "#ooooo####", "#oo#oo#", "#o# #oo#",
    "##  #oo#", "     #oo#", "     #oo#", "      ##",
};

static void draw_cursor(int mx, int my)
{
    for (int r = 0; r < (int)(sizeof(cursor_shape) / sizeof(*cursor_shape)); r++)
        for (int c = 0; cursor_shape[r][c]; c++) {
            if (cursor_shape[r][c] == '#') fb_rect(mx + c, my + r, 1, 1, 0x000000);
            else if (cursor_shape[r][c] == 'o') fb_rect(mx + c, my + r, 1, 1, 0xffffff);
        }
}

/* ---- input routing -------------------------------------------------- */

static int topmost_at(int mx, int my)      /* front-to-back hit test */
{
    for (int i = nwin - 1; i >= 0; i--) {
        struct window *win = &wins[zorder[i]];
        if (win->state == WS_NORMAL &&
            mx >= win->x && mx < win->x + win->w &&
            my >= win->y && my < win->y + win->h)
            return zorder[i];
    }
    return -1;
}

static int dragging = -1, grab_dx, grab_dy;

static void handle_input(int mx, int my, uint32_t buttons)
{
    if (!(buttons & MOUSE_BTN_LEFT)) {
        dragging = -1;
        return;
    }
    if (!frame_click) {                 /* button held: continue any drag */
        if (dragging >= 0) {
            struct window *win = &wins[dragging];
            win->x = mx - grab_dx;
            win->y = my - grab_dy;
            if (win->x < -win->w + 90) win->x = -win->w + 90;
            if (win->x > FB_WIDTH - 90) win->x = FB_WIDTH - 90;
            if (win->y < 28) win->y = 28;
            if (win->y > FB_HEIGHT - 50) win->y = FB_HEIGHT - 50;
        }
        return;
    }

    /* a fresh press anywhere unfocuses the address bar; body_browser
     * re-focuses it during this same frame's paint if the click was on
     * the field. (So clicking away sends typing back to the shell.) */
    browser_addr_blur();

    /* a fresh press: the dock gets first refusal (drawn on top), then
     * the topmost window under the cursor. */
    click_target = topmost_at(mx, my);
    if (click_target < 0)
        return;

    raise_win(click_target);
    struct window *win = &wins[click_target];

    if (my < win->y + TITLE_H) {        /* a title-bar click */
        int light = light_hit(win, mx, my);
        if (light == 0)      win->state = WS_CLOSED;        /* red */
        else if (light == 1) win->state = WS_MINIMIZED;     /* yellow */
        else if (light == 2) toggle_zoom(win);              /* green */
        else { dragging = click_target; grab_dx = mx - win->x; grab_dy = my - win->y; }
    }
    /* body clicks fall through to ui_button during draw (occlusion via
     * click_target). */
}

/* ---- the GUI task --------------------------------------------------- */

static void gui_task(uint64_t arg)
{
    (void)arg;
    int last_mx = -1, last_my = -1;
    uint32_t last_buttons = 0, last_con = (uint32_t)-1, last_ed = (uint32_t)-1;
    uint64_t last_paint = 0;

    for (;;) {
        int mx = -100, my = -100;
        uint32_t buttons = 0;
        if (vinput_present())
            mouse_state(&mx, &my, &buttons);

        frame_mx = mx;
        frame_my = my;
        frame_click = (buttons & MOUSE_BTN_LEFT) && !(last_buttons & MOUSE_BTN_LEFT);
        click_target = -1;

        if (vinput_present()) {
            if (!handle_menubar())          /* the menu bar gets first refusal */
                handle_input(mx, my, buttons);
        }
        /* the dock may also claim a click; it checks during draw below */

        /* scroll wheel scrolls whichever page is under the cursor */
        int wheel = vinput_present() ? mouse_wheel() : 0;
        if (wheel && browser_win >= 0) {
            struct window *bw = &wins[browser_win];
            if (bw->state == WS_NORMAL && mx >= bw->x && mx < bw->x + bw->w &&
                my >= bw->y && my < bw->y + bw->h)
                browser_scroll(-wheel * 40);    /* one notch ≈ 40px */
        }

        /* The spectrum analyser needs to animate, so repaint faster while
         * music is playing; otherwise idle at 4 fps to save the CPU. */
        uint64_t quiet = (player_get_state() == PS_PLAYING) ? TIMER_HZ / 20
                                                            : TIMER_HZ / 4;
        int dirty = (mx != last_mx || my != last_my || buttons != last_buttons ||
                     wheel != 0 || con_version != last_con ||
                     editor_version() != last_ed ||
                     timer_ticks() - last_paint >= quiet);
        if (dirty) {
            draw_wallpaper();
            draw_menu_bar();
            int focus = focused_win();
            for (int i = 0; i < nwin; i++) {        /* back to front */
                struct window *win = &wins[zorder[i]];
                if (win->state != WS_NORMAL)
                    continue;
                cur_body = zorder[i];
                draw_window(win, zorder[i] == focus);
            }
            draw_menus();           /* the menu-bar dropdowns sit above windows */
            draw_dock();
            if (vinput_present())
                draw_cursor(mx, my);
            fb_flip();
            frames++;
            last_mx = mx; last_my = my; last_buttons = buttons;
            last_con = con_version; last_ed = editor_version();
            last_paint = timer_ticks();
        }
        task_sleep(1);
    }
}

void gui_init(void)
{
    if (fb_init() < 0) {
        kprintf("[boot] no ramfb display (run with -device ramfb for the GUI)\n");
        return;
    }

    int term  = win_add("MicroOS - Terminal", 560, 56, 452, 560, 1, body_console, 0);
    int web   = win_add("Browser", 120, 80, 560, 540, 0, body_browser, 7);
    int files = win_add("Files", 60, 120, 420, 450, 0, body_files, 6);
    int mon   = win_add("System Monitor", 20, 60, 380, 340, 0, body_monitor, 1);
    int fontw = win_add("Fonts", 170, 130, 470, 360, 0, body_fonts, 8);
    int play  = win_add("Music", 250, 90, 360, 386, 1, body_player, 9);
    int edit  = win_add("TextEdit", 200, 110, 480, 420, 0, body_editor, 10);
    int prev  = win_add("Preview", 280, 120, 460, 400, 1, body_imgview, 11);
    int help  = win_add("Help", 150, 250, 360, 200, 0, body_help, 2);
    int about = win_add("About MicroOS", 332, 150, 360, 320, 0, body_about, 2);
    fm_cwd = fs_root;
    editor_win = edit;
    imgview_win = prev;
    about_win  = about;     /* opened from the  menu, not the dock */
    fonts_win  = fontw;     /* the App menu's "Settings…" opens this */
    wins[term].app  = "Terminal";   /* nicer than "MicroOS - Terminal" */
    wins[about].app = "MicroOS";    /* the About box belongs to the system */
    /* open a starter file if one exists, else an empty buffer */
    editor_open(fs_resolve(fs_root, "/notes.txt"));
    browser_win = web;                  /* remember it for address-bar key routing */

    /* Start with a clean desktop: every window CLOSED. The dock opens an
     * app (and raises it) when you click its icon; the red traffic light
     * closes it again. Windows keep their geometry while closed, so they
     * reopen where they were. */
    for (int i = 0; i < nwin; i++)
        wins[i].state = WS_CLOSED;

    dock[ndock++] = (struct dock_item){ "Browser",  0x2a7de1, web,   L_NONE };
    dock[ndock++] = (struct dock_item){ "Files",    0xe0a83a, files, L_NONE };
    dock[ndock++] = (struct dock_item){ "Terminal", 0x2c2c30, term,  L_NONE };
    dock[ndock++] = (struct dock_item){ "Music",    0xff2d55, play,  L_NONE };
    dock[ndock++] = (struct dock_item){ "TextEdit", 0x34c759, edit,  L_NONE };
    dock[ndock++] = (struct dock_item){ "Preview",  0x5856d6, prev,  L_NONE };
    dock[ndock++] = (struct dock_item){ "Fonts",    0x8e5acb, fontw, L_NONE };
    dock[ndock++] = (struct dock_item){ "Monitor",  0x007aff, mon,   L_NONE };
    dock[ndock++] = (struct dock_item){ "Help",     0x8e8e93, help,  L_NONE };
    dock[ndock++] = (struct dock_item){ "Beep",     0xff9500, -1,    L_BEEP };
    dock[ndock++] = (struct dock_item){ "Ping",     0x30b0c7, -1,    L_PING };
    dock[ndock++] = (struct dock_item){ "Spawn",    0x8e8e93, -1,    L_SPAWN };

    active = 1;
    task_create("gui", gui_task, 0);
    kprintf("[boot] display: %dx%d %s, window manager — GUI task started\n",
            FB_WIDTH, FB_HEIGHT, BOARD_FB_MAILBOX ? "framebuffer" : "ramfb");
}
