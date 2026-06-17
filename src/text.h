#pragma once

/*
 * text.h — the system's UTF-8, Arabic-aware text drawing.
 *
 * fb_text() draws plain ASCII with the 8x8 font. fb_text_utf8() is the
 * grown-up version: it decodes UTF-8 and draws each run in the right way
 * — Latin/ASCII with the 8x8 font, Arabic shaped right-to-left with the
 * 16x16 glyph font, accented Latin folded to its base letter. Anywhere
 * the GUI shows text that might be Arabic (file names, the terminal),
 * it calls this instead of fb_text, so the whole system speaks Arabic.
 */

#include <stdint.h>

/* Decode one UTF-8 sequence at *p (advancing it); returns the codepoint. */
int  utf8_cp(const char **p);

/* Map a non-ASCII codepoint to ASCII (Latin accents -> base letter,
 * symbols -> approximations, unknown -> '?'); returns the char count. */
int  translit(int cp, char *out);

/* Draw / measure a UTF-8 string with Arabic + Latin support. */
void fb_text_utf8(int x, int y, const char *s, uint32_t color, int scale);
int  fb_text_utf8_width(const char *s, int scale);

/* Proportional Latin font (the browser's body text, ~13px; scale for
 * headings). Draw returns the pixel width advanced. */
int  fb_prop(int x, int y, const char *s, uint32_t color, int scale);
int  fb_prop_width(const char *s, int scale);
