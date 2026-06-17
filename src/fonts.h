#pragma once

/* The system font manager: a small registry of bitmap fonts, with the
 * active one driving every fb_text() call. New fonts can be loaded from
 * a ".mf8" file (1024 hex bytes = 128 glyphs x 8 rows), downloaded over
 * plain HTTP. (Vector fonts — TTF from Google Fonts — would need TLS to
 * fetch and a rasterizer to draw; bitmaps keep it honest and tiny.) */

void  fonts_init(void);
int   fonts_count(void);
const char *fonts_name(int i);
int   fonts_active(void);
void  fonts_set(int i);
const unsigned char (*fonts_glyphs(int i))[8];   /* for per-font previews */

/* Parse .mf8 text (hex bytes) into a new font; returns its index or -1. */
int   fonts_load_mf8(const char *name, const char *data, int len);
/* Serialize a font as .mf8 text into buf; returns length written. */
int   fonts_save_mf8(int idx, char *buf, int max);
