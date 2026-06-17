#pragma once

/*
 * arabic.h — Arabic text shaping, shared across the system.
 *
 * Arabic letters change shape by position (isolated / initial / medial /
 * final) and run right-to-left. This turns a logical-order run of Arabic
 * codepoints into the glyph indices (into arabicfont.c) that the renderer
 * draws. Used by both the browser and the GUI's text routines, so Arabic
 * shows anywhere the system draws text.
 */

#include <stdint.h>

/* Index into the arabic_letters table for codepoint cp, or -1 if cp is
 * not an Arabic letter we render (callers then fall back to translit). */
int arabic_idx(int cp);

/* Shape `n` logical-order codepoints into glyph indices (returns count).
 * The caller reverses them for right-to-left painting. */
int shape_arabic(const int *cps, int n, uint8_t *out);
