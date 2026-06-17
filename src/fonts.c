/*
 * fonts.c — the system font manager.
 *
 * A "font" here is a bitmap: 128 glyphs, each 8 rows of 8 bits (the
 * same shape as the built-in font.c). The manager keeps a little
 * registry of them and tracks the ACTIVE one; fb_set_font() points the
 * renderer at its glyphs, so selecting a font re-skins the whole
 * desktop on the next repaint — no per-text-call changes needed.
 *
 * Two fonts beyond the original are generated from it at startup, which
 * is a fun lesson in what a "typeface" is at the pixel level:
 *   Bold   — OR each glyph row with itself shifted one column (every
 *            stroke gets one pixel fatter).
 *   Italic — shift each row sideways by an amount that grows toward the
 *            top, so vertical strokes lean (a "shear" — exactly how
 *            real renderers fake oblique from an upright face).
 *
 * New fonts arrive as ".mf8" files: 1024 hex bytes of glyph data,
 * downloadable over plain HTTP (fontget). Real Google Fonts can't be
 * used — they're HTTPS-only and ship vector outlines that need a
 * TrueType rasterizer — so we keep to honest, tiny bitmaps.
 */

#include "fonts.h"
#include "font.h"
#include "fb.h"
#include "lib.h"

#define MAXFONTS 8

struct sysfont {
    char          name[16];
    unsigned char g[128][8];
};

static struct sysfont fonts[MAXFONTS];
static int nfonts;
static int active;

static void set_name(char *dst, const char *s)
{
    int i = 0;
    while (s[i] && i < 15) { dst[i] = s[i]; i++; }
    dst[i] = '\0';
}

void fonts_init(void)
{
    /* 0: Classic — the original IBM-style 8x8 face */
    set_name(fonts[0].name, "Classic");
    memcpy(fonts[0].g, font8x8, sizeof(font8x8));

    /* 1: Bold — thicken every stroke by one column */
    set_name(fonts[1].name, "Bold");
    for (int c = 0; c < 128; c++)
        for (int r = 0; r < 8; r++) {
            unsigned b = font8x8[c][r];
            fonts[1].g[c][r] = (unsigned char)(b | (b << 1));
        }

    /* 2: Italic — shear the glyph: top rows shifted right the most */
    set_name(fonts[2].name, "Italic");
    for (int c = 0; c < 128; c++)
        for (int r = 0; r < 8; r++) {
            unsigned b = font8x8[c][r];
            int shift = (7 - r) / 3;
            fonts[2].g[c][r] = (unsigned char)((b << shift) & 0xFF);
        }

    nfonts = 3;
    active = 0;
    fb_set_font(fonts[0].g);
}

int  fonts_count(void)        { return nfonts; }
const char *fonts_name(int i) { return (i >= 0 && i < nfonts) ? fonts[i].name : ""; }
int  fonts_active(void)       { return active; }

const unsigned char (*fonts_glyphs(int i))[8]
{
    return (i >= 0 && i < nfonts) ? fonts[i].g : fonts[0].g;
}

void fonts_set(int i)
{
    if (i < 0 || i >= nfonts)
        return;
    active = i;
    fb_set_font(fonts[i].g);
}

/* ---- the .mf8 format: 1024 hex bytes (128 glyphs x 8 rows) --------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int fonts_load_mf8(const char *name, const char *data, int len)
{
    if (nfonts >= MAXFONTS)
        return -1;
    struct sysfont *f = &fonts[nfonts];
    memset(f->g, 0, sizeof(f->g));

    int byte = 0;                       /* which of the 1024 bytes */
    int i = 0;
    while (i < len && byte < 1024) {
        char c = data[i];
        if (c == '#' || c == ';') {     /* comment to end of line */
            while (i < len && data[i] != '\n') i++;
            continue;
        }
        if (c == '0' && i + 1 < len && (data[i+1] == 'x' || data[i+1] == 'X')) {
            i += 2;                     /* skip a 0x prefix */
            continue;
        }
        int hi = hexval(c);
        if (hi < 0) { i++; continue; }  /* whitespace / commas */
        int lo = (i + 1 < len) ? hexval(data[i + 1]) : -1;
        if (lo < 0) { i++; continue; }
        f->g[byte / 8][byte % 8] = (unsigned char)(hi * 16 + lo);
        byte++;
        i += 2;
    }
    if (byte == 0)
        return -1;                      /* not a font */

    set_name(f->name, name);
    return nfonts++;
}

int fonts_save_mf8(int idx, char *buf, int max)
{
    if (idx < 0 || idx >= nfonts)
        return 0;
    static const char hexd[] = "0123456789abcdef";
    int n = 0;
    const unsigned char *g = &fonts[idx].g[0][0];
    for (int b = 0; b < 1024 && n < max - 4; b++) {
        buf[n++] = hexd[g[b] >> 4];
        buf[n++] = hexd[g[b] & 0xF];
        buf[n++] = (b % 16 == 15) ? '\n' : ' ';
    }
    buf[n] = '\0';
    return n;
}
