/*
 * text.c — system-wide UTF-8 text drawing with Arabic support.
 *
 * Splits a UTF-8 string into runs and draws each appropriately: Arabic
 * letters are shaped (arabic.c) and painted right-to-left with the 16x16
 * glyph font; everything else is transliterated to ASCII and drawn with
 * the 8x8 font. Latin text is unaffected (it passes straight through).
 * This is what makes the desktop — file names, the terminal — speak
 * Arabic, not just the browser.
 */

#include "text.h"
#include "arabic.h"
#include "arabicfont.h"
#include "propfont.h"
#include "fb.h"

#define ARAB_ADV 11     /* horizontal advance per Arabic glyph (px) */

int utf8_cp(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    unsigned char b = *p++;
    int cp, n;
    if      (b < 0x80)         { *pp = (const char *)p; return b; }
    else if ((b & 0xE0)==0xC0) { cp = b & 0x1F; n = 1; }
    else if ((b & 0xF0)==0xE0) { cp = b & 0x0F; n = 2; }
    else if ((b & 0xF8)==0xF0) { cp = b & 0x07; n = 3; }
    else                       { *pp = (const char *)p; return 0xFFFD; }
    while (n-- && (*p & 0xC0)==0x80) cp = (cp << 6) | (*p++ & 0x3F);
    *pp = (const char *)p;
    return cp;
}

int translit(int cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }

    switch (cp) {                                /* typographic punctuation */
    case 0x2018: case 0x2019: case 0x201A: out[0]='\''; return 1;
    case 0x201C: case 0x201D: case 0x201E: out[0]='"';  return 1;
    case 0x2013: case 0x2014: case 0x2212: out[0]='-';  return 1;
    case 0x00A0: out[0]=' '; return 1;
    case 0x00B7: case 0x2022: out[0]='*'; return 1;
    case 0x00D7: out[0]='x'; return 1;
    case 0x2026: out[0]='.'; out[1]='.'; out[2]='.'; return 3;
    case 0x00A9: out[0]='('; out[1]='c'; out[2]=')'; return 3;
    case 0x00AE: out[0]='('; out[1]='r'; out[2]=')'; return 3;
    case 0x2122: out[0]='T'; out[1]='M'; return 2;
    }

    if (cp >= 0xC0 && cp <= 0xFF) {              /* Latin-1 accents -> base */
        static const char base[] =
            "AAAAAA?CEEEEIIIIDNOOOOO?OUUUUY??aaaaaa?ceeeeiiiidnooooo?ouuuuy?y";
        out[0] = base[cp - 0xC0];
        return 1;
    }

    if (cp >= 0x0660 && cp <= 0x0669) { out[0]='0'+(cp-0x0660); return 1; }  /* ٠-٩ */
    if (cp >= 0x06F0 && cp <= 0x06F9) { out[0]='0'+(cp-0x06F0); return 1; }
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x064B && cp <= 0x0652) || cp == 0x0640 || cp == 0x0670)
        return 0;                                /* harakat / tatweel / combining */

    const char *r = 0;                           /* Arabic letters (fallback) */
    switch (cp) {
    case 0x0621: case 0x0623: case 0x0625: case 0x0624: case 0x0626: r="'"; break;
    case 0x0627: case 0x0622: case 0x0649: r="a"; break;
    case 0x0628: r="b"; break;  case 0x062A: r="t"; break;  case 0x062B: r="th"; break;
    case 0x062C: r="j"; break;  case 0x062D: r="h"; break;  case 0x062E: r="kh"; break;
    case 0x062F: r="d"; break;  case 0x0630: r="dh"; break; case 0x0631: r="r"; break;
    case 0x0632: r="z"; break;  case 0x0633: r="s"; break;  case 0x0634: r="sh"; break;
    case 0x0635: r="s"; break;  case 0x0636: r="d"; break;  case 0x0637: r="t"; break;
    case 0x0638: r="z"; break;  case 0x0639: r="'"; break;  case 0x063A: r="gh"; break;
    case 0x0641: r="f"; break;  case 0x0642: r="q"; break;  case 0x0643: r="k"; break;
    case 0x0644: r="l"; break;  case 0x0645: r="m"; break;  case 0x0646: r="n"; break;
    case 0x0647: case 0x0629: r="h"; break;     case 0x0648: r="w"; break;
    case 0x064A: r="y"; break;
    case 0x060C: r=","; break;  case 0x061B: r=";"; break;  case 0x061F: r="?"; break;
    }
    if (r) { int i = 0; while (r[i]) { out[i] = r[i]; i++; } return i; }

    out[0] = '?'; return 1;                      /* unknown non-Latin glyph */
}

/* ---- the proportional Latin font (the browser's body text) --------- */

/* Draw an ASCII string in the proportional font at (x,y); `scale` blocks
 * each pixel (1 = ~13px body, 2 = headings). Returns the width drawn. */
int fb_prop(int x, int y, const char *s, uint32_t color, int scale)
{
    int x0 = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) { x += 5 * scale; continue; }
        fb_glyph16_scaled(x, y, prop_glyph[c - 0x20], color, scale);
        x += prop_adv[c - 0x20] * scale;
    }
    return x - x0;
}

int fb_prop_width(const char *s, int scale)
{
    int w = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        w += (c < 0x20 || c > 0x7E) ? 5 * scale : prop_adv[c - 0x20] * scale;
    }
    return w;
}

/* Draw an Arabic run (logical-order codepoints) right-to-left at x; the
 * shaped glyphs are reversed so painting left-to-right reads RTL.
 * Returns the advance in pixels. */
static int draw_arabic(int x, int y, const int *cps, int n, uint32_t color)
{
    uint8_t g[64];
    if (n > 64) n = 64;
    int m = shape_arabic(cps, n, g);
    for (int i = m - 1; i >= 0; i--) {           /* reversed = RTL */
        fb_glyph16(x, y - 4, arabic_glyphs[g[i]], color);
        x += ARAB_ADV;
    }
    return m * ARAB_ADV;
}

void fb_text_utf8(int x, int y, const char *s, uint32_t color, int scale)
{
    while (*s) {
        const char *t = s;
        int cp = utf8_cp(&t);
        if (arabic_idx(cp) >= 0) {               /* an Arabic run */
            int cps[64], n = 0;
            while (*s) {
                const char *u = s; int c = utf8_cp(&u);
                if (arabic_idx(c) < 0) break;
                if (n < 64) cps[n++] = c;
                s = u;
            }
            x += draw_arabic(x, y, cps, n, color);
        } else {                                 /* a Latin/other run */
            char buf[160]; int bl = 0;
            while (*s && bl < 150) { const char *u = s; int c = utf8_cp(&u);
                                     if (arabic_idx(c) >= 0) break;
                                     char os[8]; int ol = translit(c, os);
                                     for (int k = 0; k < ol && bl < 150; k++) buf[bl++] = os[k];
                                     s = u; }
            buf[bl] = 0;
            fb_text(x, y, buf, color, scale);
            x += fb_text_width(buf, scale);
        }
    }
}

int fb_text_utf8_width(const char *s, int scale)
{
    int x = 0;
    while (*s) {
        const char *t = s;
        int cp = utf8_cp(&t);
        if (arabic_idx(cp) >= 0) {
            int n = 0;
            while (*s) {
                const char *u = s; int c = utf8_cp(&u);
                if (arabic_idx(c) < 0) break;
                n++; s = u;
            }
            x += n * ARAB_ADV;
        } else {
            char buf[160]; int bl = 0;
            while (*s && bl < 150) { const char *u = s; int c = utf8_cp(&u);
                                     if (arabic_idx(c) >= 0) break;
                                     char os[8]; int ol = translit(c, os);
                                     for (int k = 0; k < ol && bl < 150; k++) buf[bl++] = os[k];
                                     s = u; }
            buf[bl] = 0;
            x += fb_text_width(buf, scale);
        }
    }
    return x;
}
