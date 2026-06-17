#pragma once
#include <stdint.h>

/* A 16x16 Arabic glyph bitmap font + letter table (GENERATED).
 * Each glyph row is 16 bits, LSB = leftmost pixel. */
struct arabic_letter { int base; uint8_t cls; uint8_t iso,fin,init,med; };
extern const uint16_t arabic_glyphs[][16];
extern const uint8_t  arabic_advance[];
extern const int      arabic_nglyph;
extern const struct arabic_letter arabic_letters[];
extern const int      arabic_nletters;
