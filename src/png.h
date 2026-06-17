#pragma once

/*
 * png.h — decode a PNG image to pixels.
 *
 * A PNG is a signature, then a stream of typed "chunks": IHDR (size and
 * format), IDAT (the pixels, zlib-compressed), PLTE (a palette), IEND.
 * Decoding is: concatenate the IDAT chunks, zlib-inflate them
 * (inflate.c) to get filtered scanlines, then "un-filter" each line —
 * PNG pre-processes rows with one of five predictors (None/Sub/Up/
 * Average/Paeth) so similar neighbouring pixels compress better, and we
 * reverse that. Finally we turn each pixel into 0xRRGGBB, compositing
 * any transparency over white.
 *
 * Supported: 8-bit depth, colour types grayscale / RGB / palette /
 * gray+alpha / RGBA, non-interlaced. (Plenty for real web PNGs; 16-bit
 * and Adam7 interlacing are left out.)
 */

#include <stdint.h>

/* Decode into `out` (0x00RRGGBB per pixel, row-major). out must hold at
 * least maxpx pixels. Returns 0 and sets *w,*h on success; -1 otherwise
 * (unsupported format, too large for maxpx, or corrupt). */
int png_decode(const uint8_t *data, int len, uint32_t *out, int maxpx,
               int *w, int *h);
