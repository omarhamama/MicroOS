/*
 * browser.c — a tiny web browser (HTTP + a subset of HTML).
 *
 * This is the web of about 1993, rebuilt from scratch: fetch a page
 * over HTTP (http.c, on our own TCP stack), parse a useful subset of
 * HTML, flow the text into lines, draw it in a window, and make the
 * links clickable. No CSS, no JavaScript, no images, and no HTTPS
 * (that needs TLS) — but real pages over plain HTTP render and you can
 * click your way around. The same shape as a real browser, three
 * stages deep:
 *
 *   PARSE   HTML text -> a flat list of styled "runs" (words + breaks)
 *   LAYOUT  runs -> positioned glyphs, wrapping at the window width
 *           (this is a browser's "reflow"; resize the window and it
 *           re-wraps)
 *   PAINT   draw them, remembering where each link landed so a click
 *           can follow it
 *
 * Fetching blocks (TCP round-trips), so it runs in its OWN task; the
 * GUI paint loop only ever reads the finished page, never waits on the
 * network.
 */

#include "browser.h"
#include "http.h"
#include "css.h"
#include "js.h"
#include "png.h"
#include "jpeg.h"
#include "arabicfont.h"
#include "arabic.h"
#include "text.h"
#include "fb.h"
#include "mem.h"
#include "task.h"
#include "lib.h"
#include "kprintf.h"

/* ---- the parsed page: a flat list of runs --------------------------- */

enum { W_NORMAL, W_LINK };      /* link gets an underline + a hit-box */

#define DEF_TEXT  0x1d1d1f

struct run {
    uint8_t  kind;      /* 0 = word, 1 = line break, 2 = paragraph break */
    uint8_t  style;     /* W_NORMAL / W_LINK */
    int16_t  href;      /* index into hrefs[], or -1 */
    uint32_t color;     /* computed text color (from the cascade) */
    uint8_t  scale;     /* computed font scale 1..3 */
    uint8_t  bold;      /* computed font-weight (faked by double-draw) */
    int16_t  elem_id;   /* id-table index this text belongs to, or -1 (for the DOM) */
    int16_t  img;       /* image-table index if this run is an <img>, else -1 */
    uint8_t  arab;      /* >0: this run is `arab` Arabic glyph indices (RTL), in text[] */
    char     text[48];
};

#define MAX_RUNS  4000
#define MAX_HREFS 400
#define HREF_BUF  16384

static struct run runs[MAX_RUNS];
static int        nruns;
static char       href_buf[HREF_BUF];
static int        href_off;
static int        href_start[MAX_HREFS];
static int        nhrefs;

static volatile int parsing;        /* GUI shows "Loading…" while set */

/* ---- the DOM glue: element ids + captured scripts ------------------- */

#define MAX_IDS 64
static char id_tab[MAX_IDS][32];
static int  n_ids;
static int  cur_elem_id = -1;       /* set during parse, stamped onto runs */
static char script_buf[16384];      /* all <script> contents, concatenated */
static int  script_len;

static int id_index(const char *name)
{
    for (int i = 0; i < n_ids; i++)
        if (!strcmp(id_tab[i], name)) return i;
    if (n_ids >= MAX_IDS) return -1;
    int i = 0;
    while (name[i] && i < 31) { id_tab[n_ids][i] = name[i]; i++; }
    id_tab[n_ids][i] = 0;
    return n_ids++;
}

/* ---- inline images: <img src> fetched + PNG-decoded after parse ----- */
#define MAX_IMG 4
struct img_slot { char src[200]; uint32_t *px; int w, h; int done; };
static struct img_slot images[MAX_IMG];
static int nimg;

/* ---- current location + history ------------------------------------- */

static char cur_url[256];
static char history[16][256];
static int  hist_n;

/* ---- link hit-boxes recorded during paint --------------------------- */

struct link_rect { int x, y, w, h, href; };
#define MAX_LRECT 512
static struct link_rect lrects[MAX_LRECT];
static int nlrect;

/* ================================================================== */
/* PARSE                                                               */
/* ================================================================== */

static int href_add(const char *s, int len)
{
    if (nhrefs >= MAX_HREFS || href_off + len + 1 > HREF_BUF)
        return -1;
    int idx = nhrefs;
    href_start[nhrefs++] = href_off;
    for (int i = 0; i < len; i++)
        href_buf[href_off++] = s[i];
    href_buf[href_off++] = '\0';
    return idx;
}

static void run_break(int para)
{
    /* collapse: don't stack breaks; upgrade to paragraph if asked */
    if (nruns && runs[nruns - 1].kind != 0) {
        if (para)
            runs[nruns - 1].kind = 2;
        return;
    }
    if (nruns < MAX_RUNS) {
        runs[nruns].kind = para ? 2 : 1;
        runs[nruns].style = W_NORMAL;
        runs[nruns].href = -1;
        runs[nruns].text[0] = 0;
        nruns++;
    }
}

static void run_word(const char *w, int len, int style, int href,
                     uint32_t color, int scale, int bold)
{
    if (!len || nruns >= MAX_RUNS)
        return;
    if (len > 47)
        len = 47;
    struct run *r = &runs[nruns++];
    r->kind = 0;
    r->style = (uint8_t)style;
    r->href = (int16_t)href;
    r->color = color;
    r->scale = (uint8_t)scale;
    r->bold = (uint8_t)bold;
    r->elem_id = (int16_t)cur_elem_id;
    r->img = -1;
    r->arab = 0;
    for (int i = 0; i < len; i++)
        r->text[i] = w[i];
    r->text[len] = 0;
}

/* Translate one HTML entity at *p (just past '&') to a Unicode codepoint,
 * advancing *p past the ';'. Non-ASCII codepoints get romanised later by
 * translit(). Handles named entities + numeric (&#1604; / &#x644;). */
static int entity(const char **p)
{
    const char *s = *p;
    int out = '&';
    if      (!strncmp(s, "amp;", 4))  { out = '&';    s += 4; }
    else if (!strncmp(s, "lt;", 3))   { out = '<';    s += 3; }
    else if (!strncmp(s, "gt;", 3))   { out = '>';    s += 3; }
    else if (!strncmp(s, "quot;", 5)) { out = '"';    s += 5; }
    else if (!strncmp(s, "apos;", 5)) { out = '\'';   s += 5; }
    else if (!strncmp(s, "#39;", 4))  { out = '\'';   s += 4; }
    else if (!strncmp(s, "nbsp;", 5)) { out = ' ';    s += 5; }
    else if (!strncmp(s, "mdash;", 6)){ out = 0x2014; s += 6; }
    else if (!strncmp(s, "ndash;", 6)){ out = 0x2013; s += 6; }
    else if (!strncmp(s, "lsquo;", 6)){ out = 0x2018; s += 6; }
    else if (!strncmp(s, "rsquo;", 6)){ out = 0x2019; s += 6; }
    else if (!strncmp(s, "ldquo;", 6)){ out = 0x201C; s += 6; }
    else if (!strncmp(s, "rdquo;", 6)){ out = 0x201D; s += 6; }
    else if (!strncmp(s, "hellip;",7)){ out = 0x2026; s += 7; }
    else if (!strncmp(s, "middot;",7)){ out = 0x00B7; s += 7; }
    else if (!strncmp(s, "bull;", 5)) { out = 0x2022; s += 5; }
    else if (!strncmp(s, "times;", 6)){ out = 0x00D7; s += 6; }
    else if (!strncmp(s, "copy;", 5)) { out = 0x00A9; s += 5; }
    else if (!strncmp(s, "reg;", 4))  { out = 0x00AE; s += 4; }
    else if (!strncmp(s, "trade;", 6)){ out = 0x2122; s += 6; }
    else if (!strncmp(s, "deg;", 4))  { out = 0x00B0; s += 4; }
    else if (!strncmp(s, "euro;", 5)) { out = 0x20AC; s += 5; }
    else if (!strncmp(s, "pound;", 6)){ out = 0x00A3; s += 6; }
    else if (s[0] == '#') {                     /* numeric character reference */
        long v = 0; s++;
        if (*s == 'x' || *s == 'X') { s++;
            while ((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')||(*s>='A'&&*s<='F')) {
                int d = *s<='9'?*s-'0':(*s|32)-'a'+10; v = v*16+d; s++;
            }
        } else while (*s>='0'&&*s<='9') v = v*10 + (*s++ - '0');
        if (*s == ';') s++;
        out = (int)v;
    }
    *p = s;
    return out;
}

/* Emit one Arabic word as a run: shape it, then store the glyph indices
 * REVERSED so painting them left-to-right yields right-to-left text. */
static void emit_arabic(const int *cps, int n, int style, int href,
                        uint32_t color, int elemid)
{
    if (nruns >= MAX_RUNS || n <= 0) return;
    if (n > 47) n = 47;
    uint8_t g[48];
    int m = shape_arabic(cps, n, g);
    if (!m) return;
    struct run *r = &runs[nruns++];
    r->kind = 0; r->style = (uint8_t)style; r->href = (int16_t)href;
    r->color = color; r->scale = 1; r->bold = 0;
    r->elem_id = (int16_t)elemid; r->img = -1; r->arab = (uint8_t)m;
    for (int i = 0; i < m; i++) r->text[i] = (char)g[m-1-i];   /* RTL */
    r->text[m] = 0;
}

/* Find attribute `name`'s value within a tag's text [tag, end); copy to
 * out (NUL-terminated). Returns 1 if found. */
static int get_attr(const char *tag, const char *end, const char *name,
                    char *out, int max)
{
    int nl = (int)strlen(name);
    for (const char *h = tag; h + nl < end; h++) {
        if (strncmp(h, name, nl)) continue;
        if (h != tag && h[-1] != ' ' && h[-1] != '\t') continue;   /* word start */
        const char *e = h + nl;
        while (e < end && (*e == ' ' || *e == '=')) {
            if (*e == '=') { e++; goto val; }
            e++;
        }
        continue;
    val:;
        while (e < end && (*e == ' ')) e++;
        char q = (*e == '"' || *e == '\'') ? *e : 0;
        if (q) e++;
        int o = 0;
        while (e < end && o < max-1 && *e != (q ? q : ' ') && *e != '>') out[o++] = *e++;
        out[o] = 0;
        return 1;
    }
    return 0;
}

/* lowercase the tag's leading name token into out */
static void tag_name(const char *tag, const char *end, char *out, int max)
{
    int o = 0;
    if (tag < end && *tag == '/') tag++;          /* skip close-tag slash */
    while (tag < end && o < max-1) {
        char c = *tag++;
        if (c==' '||c=='>'||c=='/'||c=='\t') break;
        out[o++] = (c>='A'&&c<='Z') ? c+32 : c;
    }
    out[o] = 0;
}

/* Does p point at "name" (case-insensitive) followed by a tag boundary?
 * Used to spot </script> / </style> while reading raw script/style text. */
static int close_is(const char *p, const char *name)
{
    int i = 0;
    for (; name[i]; i++) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != name[i]) return 0;
    }
    char c = p[i];
    return c=='>' || c==' ' || c=='\t' || c=='/';
}

/* The computed visual style at the current nesting depth. */
struct sty { uint32_t color; uint8_t scale; uint8_t bold; uint8_t hidden; int16_t id; };

/* Tag default styling (before any CSS), applied over the inherited style. */
static void tag_defaults(const char *name, struct sty *s)
{
    if (!strcmp(name,"h1")) { s->scale=3; s->bold=1; }
    else if (!strcmp(name,"h2")) { s->scale=2; s->bold=1; }
    else if (!strcmp(name,"h3")||!strcmp(name,"h4")) { s->scale=2; s->bold=1; }
    else if (!strcmp(name,"b")||!strcmp(name,"strong")) s->bold=1;
    else if (!strcmp(name,"small")) s->scale=1;
    else if (!strcmp(name,"a")) s->color = 0x0050d0;   /* link blue */
}

/* Does this tag start a block (forcing a line break)? */
static int is_block(const char *n)
{
    static const char *b[] = {"p","div","br","tr","ul","ol","hr","h1","h2","h3",
        "h4","section","article","header","footer","nav","table","blockquote",0};
    for (int i = 0; b[i]; i++) if (!strcmp(n, b[i])) return 1;
    return 0;
}

static void resolve_url(const char *href, char *out, int max);  /* defined below */

/* Fetch an external <link rel="stylesheet"> and feed it to the CSS
 * engine. Done synchronously during parse (we're on the browser task),
 * so the rules are in place before the body lays out. Capped + bounded;
 * real stylesheets are huge and use selectors we don't support, so only
 * our subset (tag/.class/#id + a few properties) actually takes effect. */
static int n_ext_css;
static void fetch_stylesheet(const char *tag, const char *end)
{
    if (n_ext_css >= 3) return;
    char rel[40] = "", href[256] = "";
    get_attr(tag, end, "rel", rel, sizeof rel);
    get_attr(tag, end, "href", href, sizeof href);
    if (!href[0]) return;
    int is_css = 0;                              /* rel contains "stylesheet"? */
    for (char *r = rel; *r; r++)
        if ((r[0]|32)=='s' && !strncmp(r+1,"tylesheet",9)) { is_css = 1; break; }
    if (!is_css) return;

    char url[256];
    resolve_url(href, url, sizeof url);
    char *buf = kmalloc(64 * 1024);
    if (!buf) return;
    int n = http_get(url, buf, 64 * 1024);
    if (n > 0) css_add_stylesheet(buf, n);
    kfree(buf);
    n_ext_css++;
}

/* Parse HTML into runs. When `append` is set (document.write during
 * script execution), we keep the existing page/CSS/ids and add to them
 * and do NOT capture scripts (so written markup can't re-trigger JS). */
static void parse_html2(const char *html, int append)
{
    if (!append) {
        nruns = 0; nhrefs = 0; href_off = 0;
        css_reset();
        n_ids = 0; script_len = 0; n_ext_css = 0;
        for (int i = 0; i < nimg; i++) { if (images[i].px) kfree(images[i].px); }
        nimg = 0;
    }
    cur_elem_id = -1;

    struct sty stack[40];
    int sp = 0;
    stack[0] = (struct sty){ DEF_TEXT, 1, 0, 0, -1 };

    int link = -1;
    int skip = 0;               /* inside <head> */
    int in_style = 0, in_script = 0;
    static char css_buf[8192];
    int css_len = 0;
    char word[48];
    int wlen = 0;
    int arb[48];                /* pending Arabic codepoints (logical order) */
    int arbn = 0;

    /* End the current ASCII word, then any pending Arabic word. (Within a
     * token only one is non-empty; we switch scripts explicitly below.) */
    #define FLUSH() do { \
        if (wlen) { struct sty *t=&stack[sp]; cur_elem_id=t->id; \
            if (!t->hidden) run_word(word,wlen,link>=0?W_LINK:W_NORMAL,link,t->color,t->scale,t->bold); \
            wlen=0; } \
        if (arbn) { struct sty *t=&stack[sp]; \
            if (!t->hidden) emit_arabic(arb,arbn,link>=0?W_LINK:W_NORMAL,link,t->color,t->id); \
            arbn=0; } \
    } while (0)

    const char *p = html;
    while (*p && nruns < MAX_RUNS - 2) {
        /* raw-text modes: read everything verbatim until the close tag, so
         * a '<' inside a script (e.g. "n < 2") isn't mistaken for a tag. */
        if (in_script) {
            if (p[0]=='<' && p[1]=='/' && close_is(p+2,"script")) {
                if (script_len < (int)sizeof(script_buf)-2) script_buf[script_len++]='\n';
                in_script = 0;
                while (*p && *p != '>') p++;
                if (*p) p++;
                continue;
            }
            if (script_len < (int)sizeof(script_buf)-2) script_buf[script_len++] = *p;
            p++; continue;
        }
        if (in_style) {
            if (p[0]=='<' && p[1]=='/' && close_is(p+2,"style")) {
                css_add_stylesheet(css_buf, css_len);
                in_style = 0;
                while (*p && *p != '>') p++;
                if (*p) p++;
                continue;
            }
            if (css_len < (int)sizeof(css_buf)-1) css_buf[css_len++] = *p;
            p++; continue;
        }
        if (*p == '<') {
            FLUSH();
            p++;
            const char *tag = p;
            while (*p && *p != '>') p++;
            int closing = (tag < p && *tag == '/');
            char name[24];
            tag_name(tag, p, name, sizeof name);

            if (!strcmp(name,"head")) {
                skip = !closing;
            } else if (!strcmp(name,"style")) {
                if (!closing) { in_style = 1; css_len = 0; }
            } else if (!strcmp(name,"script")) {
                if (!closing && !append) in_script = 1;     /* capture to run later */
                else if (!closing) skip = 1;                 /* append mode: ignore */
            } else if (!strcmp(name,"link")) {              /* external stylesheet */
                if (!closing && !append) fetch_stylesheet(tag, p);
            } else if (!skip) {
                if (!strcmp(name,"img") && !closing && nimg < MAX_IMG && nruns < MAX_RUNS) {
                    char src[200] = "";
                    if (get_attr(tag, p, "src", src, sizeof src) && src[0]) {
                        int k=0; while (src[k] && k<199) { images[nimg].src[k]=src[k]; k++; }
                        images[nimg].src[k]=0; images[nimg].px=0; images[nimg].done=0;
                        struct run *r = &runs[nruns++];      /* an image placeholder run */
                        r->kind=0; r->style=W_NORMAL; r->href=(int16_t)link;
                        r->color=DEF_TEXT; r->scale=1; r->bold=0; r->arab=0;
                        r->elem_id=(int16_t)cur_elem_id; r->img=(int16_t)nimg; r->text[0]=0;
                        nimg++;
                    }
                }
                if (is_block(name)) run_break(1);
                if (!strcmp(name,"li") && !closing) {
                    cur_elem_id = stack[sp].id;
                    run_break(0);
                    run_word("\x07", 1, W_NORMAL, -1, stack[sp].color, 1, 0);
                }
                if (closing) {
                    if (!strcmp(name,"a")) link = -1;
                    if (sp > 0) sp--;
                } else if (*tag && p[-1] != '/' &&
                           strcmp(name,"br") && strcmp(name,"hr") &&
                           strcmp(name,"img") && strcmp(name,"meta") &&
                           strcmp(name,"link") && strcmp(name,"input")) {
                    struct sty ns = stack[sp];
                    tag_defaults(name, &ns);
                    char cls[64]="", id[64]="", inl[256]="";
                    get_attr(tag, p, "class", cls, sizeof cls);
                    get_attr(tag, p, "id", id, sizeof id);
                    if (id[0]) ns.id = (int16_t)id_index(id);  /* DOM handle */
                    struct css_style cs; memset(&cs,0,sizeof cs);
                    cs.bold=cs.center=cs.hidden=-1;
                    css_match(name, cls, id, &cs);
                    if (get_attr(tag, p, "style", inl, sizeof inl))
                        css_parse_inline(inl, (int)strlen(inl), &cs);
                    if (cs.has_color) ns.color = cs.color;
                    if (cs.has_size)  ns.scale = (uint8_t)cs.scale;
                    if (cs.bold>=0)   ns.bold = (uint8_t)cs.bold;
                    if (cs.hidden>=0) ns.hidden = (uint8_t)cs.hidden;
                    if (!strcmp(name,"a")) {
                        char href[256];
                        if (get_attr(tag, p, "href", href, sizeof href))
                            link = href_add(href, (int)strlen(href));
                    }
                    if (sp < 39) stack[++sp] = ns;
                }
            }
            if (*p == '>') p++;
            continue;
        }

        if (skip) { p++; continue; }

        /* Decode one source unit to a codepoint, then transliterate it to
         * ASCII (Arabic/accents -> Latin) — one codepoint can become a few
         * characters, so we feed each through the word/whitespace logic. */
        int cp;
        if (*p == '&') { p++; cp = entity(&p); }
        else if ((unsigned char)*p < 0x80) cp = *p++;
        else cp = utf8_cp(&p);

        if (arabic_idx(cp) >= 0) {               /* an Arabic letter: real glyphs */
            if (wlen) FLUSH();                   /* end any ASCII word first */
            if (arbn < 47) arb[arbn++] = cp;
            continue;
        }
        if (arbn) FLUSH();                       /* leaving Arabic: flush the word */

        char os[8];
        int olen = translit(cp, os);             /* digits/punct/accents -> ASCII */
        for (int oi = 0; oi < olen; oi++) {
            char c = os[oi];
            if (c==' '||c=='\t'||c=='\n'||c=='\r') FLUSH();
            else if (wlen < 47) word[wlen++] = c;
        }
    }
    FLUSH();
    #undef FLUSH
}

static void parse_html(const char *html) { parse_html2(html, 0); }

/* ================================================================== */
/* PAINT (layout + draw + record link rects)                           */
/* ================================================================== */

#define COL_PAGE   0xffffff
#define COL_TEXT   0x1d1d1f
#define COL_H1     0x111114
#define COL_H2     0x3a3a3e
#define COL_LINK   0x0050d0
#define COL_MUTED  0x86868b

/* ---- scrolling: the page can be taller than its window ------------- */
static int scroll_y;        /* current vertical scroll, in pixels */
static int content_h;       /* total laid-out content height (set each draw) */
static int view_h;          /* visible page height (set each draw) */

void browser_scroll(int dpx)
{
    scroll_y += dpx;
    int maxs = content_h - view_h;
    if (maxs < 0) maxs = 0;
    if (scroll_y > maxs) scroll_y = maxs;
    if (scroll_y < 0) scroll_y = 0;
}

void browser_draw(int x, int y, int w, int h)
{
    fb_rect(x, y, w, h, COL_PAGE);
    nlrect = 0;
    view_h = h;

    if (parsing) {
        fb_prop(x + 16, y + 16, "Loading...", COL_MUTED, 1);
        return;
    }

    int pad = 12;
    int top = y + 10;
    int cx = x + pad, cy = top - scroll_y;      /* everything offset by scroll */
    int line_h = 16;
    int bottom = y + h;

    /* Lay out ALL runs (so we can measure total height); draw only the
     * ones currently inside the [y, y+h] viewport. */
    for (int i = 0; i < nruns; i++) {
        struct run *r = &runs[i];
        if (r->kind) {                          /* a break */
            cx = x + pad;
            cy += line_h + (r->kind == 2 ? 10 : 3);
            line_h = 16;
            continue;
        }

        if (r->img >= 0) {                      /* an inline image */
            struct img_slot *im = &images[r->img];
            cx = x + pad; cy += line_h + 3;     /* images take their own line */
            int maxw = w - 2*pad;
            int dw = 120, dh = 40, isimg = 0;
            if (im->px && im->w > 0) {
                dw = im->w; dh = im->h;
                if (dw > maxw) { dh = dh * maxw / dw; dw = maxw; }
                if (dh > 240) { dw = dw * 240 / dh; dh = 240; }
                isimg = 1;
            }
            if (cy + dh > y && cy < bottom) {   /* visible? */
                if (isimg) fb_blit_scaled(x + pad, cy, dw, dh, im->px, im->w, im->h);
                else { fb_rect(x + pad, cy, 120, 40, 0xe8e8ec);
                       fb_prop(x + pad + 8, cy + 14, "[image]", COL_MUTED, 1); }
            }
            cy += (isimg ? dh : 40) + 6;
            line_h = 16;
            continue;
        }

        if (r->arab) {                          /* an Arabic word: real glyphs, RTL */
            int aw = r->arab * 11 + 4;
            if (cx + aw > x + w - pad && cx > x + pad) {    /* wrap */
                cx = x + pad; cy += line_h + 3; line_h = 16;
            }
            if (cy + 16 > y && cy < bottom) {
                int gx = cx;
                for (int gi = 0; gi < r->arab; gi++) {
                    fb_glyph16(gx, cy, arabic_glyphs[(uint8_t)r->text[gi]], r->color);
                    gx += 11;
                }
                if (r->style == W_LINK) {
                    fb_rect(cx, cy + 14, aw, 1, r->color);
                    if (r->href >= 0 && nlrect < MAX_LRECT)
                        lrects[nlrect++] = (struct link_rect){ cx, cy, aw, 16, r->href };
                }
            }
            cx += aw + 5;
            continue;
        }

        if (r->text[0] == '\x07') {             /* list bullet (indented) */
            cx = x + pad + 16;
            if (cy + 10 > y && cy < bottom)
                fb_rect(cx - 10, cy + 6, 3, 3, COL_TEXT);
            continue;
        }

        int scale = r->scale ? r->scale : 1;
        if (scale > 2) scale = 2;               /* cap heading size (prop font) */
        uint32_t color = r->color;

        int tw = fb_prop_width(r->text, scale);
        if (cx + tw > x + w - pad && cx > x + pad) {    /* wrap */
            cx = x + pad;
            cy += line_h + 3;
            line_h = 16;
        }
        int lh = 16 * scale;
        if (lh > line_h) line_h = lh;

        if (cy + 16 * scale > y && cy < bottom) {       /* visible: draw it */
            fb_prop(cx, cy, r->text, color, scale);
            if (r->bold) fb_prop(cx + 1, cy, r->text, color, scale);   /* fake bold */
            if (r->style == W_LINK) {
                fb_rect(cx, cy + 13 * scale, tw, 1, color);
                if (r->href >= 0 && nlrect < MAX_LRECT)
                    lrects[nlrect++] = (struct link_rect){ cx, cy, tw, 16*scale, r->href };
            }
        }
        cx += tw + 5;                           /* inter-word space */
    }

    if (!nruns) {
        fb_text(x + 16, y + 16, "(empty page)", COL_MUTED, 1);
        return;
    }

    /* total height + clamp + a scrollbar on the right edge */
    content_h = (cy + scroll_y) - top + line_h;
    int maxs = content_h - view_h;
    if (maxs < 0) maxs = 0;
    if (scroll_y > maxs) scroll_y = maxs;
    if (content_h > view_h) {
        int tx = x + w - 6;
        fb_rect(tx, y, 5, h, 0xeeeef2);                 /* track */
        int th = view_h * h / content_h;
        if (th < 24) th = 24;
        int ty = y + (maxs ? (int)((int64_t)scroll_y * (h - th) / maxs) : 0);
        fb_rounded(tx, ty, 5, th, 2, 0xb0b0b8);         /* thumb */
    }
}

/* ================================================================== */
/* NAVIGATION                                                          */
/* ================================================================== */

/* Resolve a possibly-relative href against the current URL into `out`. */
static void resolve_url(const char *href, char *out, int max)
{
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) {
        int i = 0;
        for (; href[i] && i < max - 1; i++) out[i] = href[i];
        out[i] = 0;
        return;
    }
    /* keep the current page's scheme for relative links */
    const char *scheme = !strncmp(cur_url, "https://", 8) ? "https://" : "http://";
    const char *u = cur_url;
    if (!strncmp(u, "https://", 8)) u += 8;
    else if (!strncmp(u, "http://", 7)) u += 7;
    char host[64];
    int hi = 0;
    while (u[hi] && u[hi] != '/' && hi < 63) { host[hi] = u[hi]; hi++; }
    host[hi] = 0;

    if (href[0] == '/')
        ksprintf(out, (unsigned long)max, "%s%s%s", scheme, host, href);   /* root-relative */
    else
        ksprintf(out, (unsigned long)max, "%s%s/%s", scheme, host, href);  /* approx relative */
}

/* The built-in home page — rendered without any network. */
static const char *home_page =
    "<h1>MicroOS Browser</h1>"
    "<p>A web browser written from scratch: it speaks HTTP and HTTPS on "
    "the kernel's own TCP stack, parses a subset of HTML, and lays the "
    "text out in this window. <b>Click the address bar above, type a URL, "
    "and press Enter</b> - or click a link below. Scroll with the mouse "
    "wheel or the arrow / Page keys.</p>"
    "<h2>Try a secure page</h2>"
    "<ul>"
    "<li><a href=\"https://community.letsencrypt.org\">community.letsencrypt.org</a> "
    "(real HTTPS, cert chained to ISRG Root X1)</li>"
    "<li><a href=\"https://www.digicert.com\">www.digicert.com</a> "
    "(HTTPS, DigiCert root)</li>"
    "<li><a href=\"http://neverssl.com\">neverssl.com</a> "
    "(plain HTTP)</li>"
    "</ul>"
    "<p><b>HTTPS works now:</b> a from-scratch TLS 1.3 client (X25519, "
    "ChaCha20-Poly1305, SHA-256) with full certificate validation - RSA "
    "and ECDSA signatures checked against built-in root CAs. The lock by "
    "the address bar turns green when the chain is trusted. Sites whose "
    "chain uses a curve we don't implement (P-384) show a red warning. "
    "Type <b>https &lt;url&gt;</b> in the Terminal too.</p>"
    "<h2>CSS + JavaScript</h2>"
    "<p>This page also runs a tiny <b>CSS</b> engine and a from-scratch "
    "<b>JavaScript</b> interpreter. The line below starts as a placeholder "
    "and is rewritten by a script (numbers are integers - no FPU):</p>"
    "<p id=\"jsout\" style=\"color: teal\">[script has not run]</p>"
    "<script>\n"
    "  function fib(n){ if (n < 2) return n; return fib(n-1) + fib(n-2); }\n"
    "  var msg = 'JS ran in the browser: fib(15) = ' + fib(15);\n"
    "  document.getElementById('jsout').textContent = msg;\n"
    "  console.log('hello from console.log, 6*7 =', 6*7);\n"
    "  document.write('<p>This paragraph was created by document.write().</p>');\n"
    "</script>";

/* request queue: the GUI/shell sets pending; the browser task fetches */
static volatile int  nav_pending;
static char          nav_url[256];

void browser_navigate(const char *url)
{
    int i = 0;
    while (url[i] && i < (int)sizeof(nav_url) - 1) { nav_url[i] = url[i]; i++; }
    nav_url[i] = 0;
    nav_pending = 1;
    task_wakeup((void *)&nav_pending);
}

void browser_click(int mx, int my)
{
    for (int i = 0; i < nlrect; i++) {
        struct link_rect *L = &lrects[i];
        if (mx >= L->x && mx < L->x + L->w && my >= L->y && my < L->y + L->h) {
            char url[256];
            resolve_url(href_buf + href_start[L->href], url, sizeof(url));
            /* remember where we are, for Back */
            if (hist_n < 16)
                for (int k = 0; cur_url[k] && k < 255; k++)
                    history[hist_n][k] = cur_url[k], history[hist_n][k + 1] = 0;
            if (hist_n < 16) hist_n++;
            browser_navigate(url);
            return;
        }
    }
}

void browser_back(void)
{
    if (hist_n > 0)
        browser_navigate(history[--hist_n]);
}

const char *browser_status(void)
{
    return parsing ? "Loading..." : cur_url;
}

/* ---- editable address bar ------------------------------------------- */
/* Click the address field to focus it; then on-screen keystrokes are
 * routed here (gui.c -> browser_addr_key) instead of to the shell. */

static int  addr_editing;
static int  addr_fresh;          /* just focused: first keystroke replaces (select-all) */
static char addr_buf[256];
static int  addr_len;
static int  addr_cur;            /* caret position within addr_buf [0..addr_len] */
static int  addr_was_editing;    /* editing state captured at the last blur */

void browser_addr_focus(void)
{
    addr_editing = 1;
    addr_fresh = 1;              /* show the URL, but the first typed char clears it */
    int i = 0;
    while (cur_url[i] && i < (int)sizeof(addr_buf) - 1) { addr_buf[i] = cur_url[i]; i++; }
    addr_buf[i] = 0;
    addr_len = i;
    addr_cur = i;               /* caret at the end */
}

/* Called on every fresh click (gui.c handle_input). We remember whether
 * we were editing so body_browser can tell a re-click on the field (keep
 * the buffer, move the caret) from a first click (focus + select-all). */
void browser_addr_blur(void) { addr_was_editing = addr_editing; addr_editing = 0; }
int  browser_addr_editing(void) { return addr_editing; }
int  browser_addr_cursor(void) { return addr_cur; }

/* What the address field should display: the edit buffer while typing,
 * otherwise the current URL (or "Loading..."). */
const char *browser_addr_text(void)
{
    return addr_editing ? addr_buf : browser_status();
}

/* Move the caret within the URL. dir: 0=left 1=right 2=home 3=end. */
void browser_addr_arrow(int dir)
{
    if (!addr_editing) return;
    addr_fresh = 0;             /* moving the caret means "edit in place", not replace */
    if (dir == 0)      { if (addr_cur > 0)       addr_cur--; }
    else if (dir == 1) { if (addr_cur < addr_len) addr_cur++; }
    else if (dir == 2) addr_cur = 0;
    else               addr_cur = addr_len;
}

/* A click landed on the address field. If we were editing right before
 * this click's blur, keep the buffer and just move the caret to where the
 * user clicked; otherwise focus the field fresh (select-all). `px` is the
 * click offset into the text (8 px per glyph). */
void browser_addr_clickfield(int px)
{
    if (addr_was_editing) {
        addr_editing = 1;
        addr_fresh = 0;
        int c = px < 4 ? 0 : (px + 4) / 8;      /* round to nearest gap */
        if (c > addr_len) c = addr_len;
        addr_cur = c;
    } else {
        browser_addr_focus();
    }
}

/* Feed one typed character to the focused address bar. Returns 1 if it
 * was consumed (so the keystroke shouldn't fall through to the shell). */
int browser_addr_key(char c)
{
    if (!addr_editing)
        return 0;
    if (c == '\r' || c == '\n') {               /* Enter: navigate */
        addr_editing = 0;
        if (addr_len) {
            if (hist_n < 16) {                  /* remember current page for Back */
                int k = 0;
                while (cur_url[k] && k < 255) { history[hist_n][k] = cur_url[k]; k++; }
                history[hist_n][k] = 0; hist_n++;
            }
            browser_navigate(addr_buf);
        }
    } else if (c == 0x7f || c == '\b') {        /* Backspace: delete before caret */
        addr_fresh = 0;
        if (addr_cur > 0) {
            memmove(addr_buf + addr_cur - 1, addr_buf + addr_cur,
                    (size_t)(addr_len - addr_cur + 1));   /* include the NUL */
            addr_len--;
            addr_cur--;
        }
    } else if (c == 0x1b) {                      /* Esc: cancel editing */
        addr_editing = 0;
    } else if (c >= ' ' && c < 0x7f && addr_len < (int)sizeof(addr_buf) - 1) {
        if (addr_fresh) { addr_len = 0; addr_cur = 0; addr_buf[0] = 0; addr_fresh = 0; }
        memmove(addr_buf + addr_cur + 1, addr_buf + addr_cur,
                (size_t)(addr_len - addr_cur + 1));       /* shift tail + NUL right */
        addr_buf[addr_cur] = c;
        addr_len++;
        addr_cur++;
    }
    return 1;
}

/* ---- the fetch task: turns a pending URL into a parsed page -------- */

static char fetch_buf[96 * 1024];

/* security state of the page now displayed, for the lock indicator */
static volatile int sec_state;          /* 0 http, 1 verified https, -1 failed */
static char         sec_text[80];

int browser_security(void) { return sec_state; }
const char *browser_security_text(void) { return sec_text; }

static void set_sec(int state, const char *text)
{
    sec_state = state;
    int i = 0;
    if (text) while (text[i] && i < (int)sizeof(sec_text)-1) { sec_text[i]=text[i]; i++; }
    sec_text[i] = 0;
}

/* ---- DOM hooks the JS interpreter calls back into -------------------- */

static void dom_write(void *cx, const char *s) { (void)cx; parse_html2(s, 1); }

static void dom_log(void *cx, const char *s)
{
    (void)cx;
    run_break(1);
    cur_elem_id = -1;
    run_word(">", 1, W_NORMAL, -1, 0x8e5acb, 1, 1);   /* console marker */
    char w[48]; int wl = 0;
    for (const char *q = s; ; q++) {
        if (*q==' ' || *q==0) {
            if (wl) { run_word(w, wl, W_NORMAL, -1, 0x515154, 1, 0); wl = 0; }
            if (!*q) break;
        } else if (wl < 47) w[wl++] = *q;
    }
}

static void dom_set(void *cx, const char *id, const char *s)
{
    (void)cx;
    int idx = id_index(id), first = 1;
    for (int i = 0; i < nruns; i++)
        if (runs[i].kind == 0 && runs[i].elem_id == idx) {
            if (first) {
                int n = (int)strlen(s); if (n > 47) n = 47;
                memcpy(runs[i].text, s, n); runs[i].text[n] = 0;
                first = 0;
            } else runs[i].text[0] = 0;      /* collapse the rest of the element */
        }
}

static int dom_get(void *cx, const char *id, char *out, int max)
{
    (void)cx;
    int idx = id_index(id), o = 0; out[0] = 0;
    for (int i = 0; i < nruns; i++)
        if (runs[i].kind == 0 && runs[i].elem_id == idx) {
            const char *t = runs[i].text;
            if (o && o < max-1) out[o++] = ' ';
            while (*t && o < max-1) out[o++] = *t++;
        }
    out[o] = 0;
    return 0;
}

/* Run any <script> blocks captured during the last parse. */
static void run_scripts(void)
{
    if (script_len <= 0) return;
    script_buf[script_len] = 0;
    struct js_host host = { dom_write, dom_log, dom_set, dom_get, 0 };
    char err[64];
    js_run(script_buf, &host, err, sizeof err);
}

/* Fetch + PNG-decode each <img> the page referenced. Runs on the
 * browser task (it blocks on the network), never in the paint loop. */
#define IMG_MAXPX 250000        /* ~500x500 ceiling per image */
static void load_images(void)
{
    if (nimg == 0) return;
    uint8_t *raw = kmalloc(512 * 1024);
    uint32_t *tmp = kmalloc(IMG_MAXPX * 4);
    if (!raw || !tmp) { if (raw) kfree(raw); if (tmp) kfree(tmp); return; }

    for (int i = 0; i < nimg; i++) {
        if (images[i].done) continue;
        images[i].done = 1;
        char url[256];
        resolve_url(images[i].src, url, sizeof url);
        int n = http_get(url, (char *)raw, 512 * 1024);
        if (n <= 0) continue;
        int w, h, ok = -1;
        if (n > 8 && raw[0] == 0x89 && raw[1] == 'P')          /* PNG signature */
            ok = png_decode(raw, n, tmp, IMG_MAXPX, &w, &h);
        else if (n > 2 && raw[0] == 0xFF && raw[1] == 0xD8)    /* JPEG SOI */
            ok = jpeg_decode(raw, n, tmp, IMG_MAXPX, &w, &h);
        /* (other formats — WebP/GIF/SVG — aren't decoded; show a placeholder) */
        if (ok == 0) {
            uint32_t *px = kmalloc((uint64_t)w * h * 4);
            if (px) { memcpy(px, tmp, (uint64_t)w * h * 4);
                      images[i].px = px; images[i].w = w; images[i].h = h; }
        }
    }
    kfree(raw); kfree(tmp);
}

static void do_load(const char *url)
{
    parsing = 1;
    scroll_y = 0;               /* every new page starts at the top */

    int i = 0;
    while (url[i] && i < (int)sizeof(cur_url) - 1) { cur_url[i] = url[i]; i++; }
    cur_url[i] = 0;

    if (!strncmp(url, "about:", 6)) {
        set_sec(0, "");
        parse_html(home_page);
        run_scripts();
        load_images();
        parsing = 0;
        return;
    }

    int https = !strncmp(url, "https://", 8);
    struct tls_info info;
    memset(&info, 0, sizeof info);
    int n = http_get_info(url, fetch_buf, sizeof(fetch_buf), &info);

    /* The lock reflects the CONNECTION's security. If the handshake +
     * certificate validated, it's green even if the page later truncated. */
    if (https) {
        if (info.ok) set_sec(1, info.anchor);
        else         set_sec(-1, info.err[0] ? info.err : "TLS handshake failed");
    } else {
        set_sec(0, "");
    }

    if (n < 0) {
        if (https && info.ok) {
            /* TLS was fine — the failure was downloading/decoding the page,
             * NOT security. Be honest about which. */
            parse_html("<h1>Page did not load</h1>"
                       "<p>The secure connection succeeded (the certificate "
                       "verified), but the page could not be downloaded - our "
                       "from-scratch TCP has no flow control, so it can stall "
                       "on very large responses. Smaller pages work. "
                       "<a href=\"about:home\">Back to home</a>.</p>");
        } else if (https) {
            char msg[256];
            ksprintf(msg, sizeof msg,
                     "<h1>Secure connection failed</h1>"
                     "<p>TLS or certificate validation failed: <b>%s</b>. "
                     "<a href=\"about:home\">Back to home</a>.</p>",
                     info.err[0] ? info.err : "unknown");
            parse_html(msg);
        } else {
            parse_html("<h1>Can't load page</h1><p>The host did not answer "
                       "(unreachable, or HTTPS-only - try https://). "
                       "<a href=\"about:home\">Back to home</a>.</p>");
        }
    } else {
        parse_html(fetch_buf);
    }
    run_scripts();
    load_images();
    parsing = 0;
}

static void browser_task(uint64_t arg)
{
    (void)arg;
    do_load("about:home");          /* open to the home page */

    for (;;) {
        asm volatile("msr daifset, #2");
        while (!nav_pending)
            task_block((void *)&nav_pending);
        nav_pending = 0;
        asm volatile("msr daifclr, #2");

        char url[256];
        for (int i = 0; (url[i] = nav_url[i]); i++)
            if (i >= 255) { url[255] = 0; break; }
        do_load(url);
    }
}

void browser_init(void)
{
    /* A roomy stack: this task runs the recursive JS interpreter and the
     * TLS handshake's big-integer crypto, both far deeper than 16 KB. */
    task_create_stack("browser", browser_task, 0, 256 * 1024);
}
