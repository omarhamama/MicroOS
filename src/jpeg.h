#pragma once

/*
 * jpeg.h — decode a baseline JPEG image to pixels.
 *
 * JPEG is lossy compression built on the Discrete Cosine Transform: an
 * image is split into 8x8 blocks, each transformed to frequencies, the
 * high frequencies (which the eye barely notices) quantised away, and
 * the result Huffman-coded. Decoding reverses that: Huffman-decode the
 * coefficients, de-quantise, run an inverse DCT back to pixels, and
 * convert the YCbCr colour the format stores into RGB.
 *
 * Supported: BASELINE JPEG (the common kind) — 8-bit, Huffman, any
 * chroma subsampling. NOT supported: progressive JPEG, arithmetic
 * coding, 12-bit. The inverse DCT is integer-only (the kernel has no
 * FPU), using a precomputed fixed-point cosine table.
 */

#include <stdint.h>

/* Decode into `out` (0x00RRGGBB per pixel, row-major); out must hold at
 * least maxpx pixels. Returns 0 and sets *w,*h on success, else -1
 * (progressive/unsupported/too large/corrupt). */
int jpeg_decode(const uint8_t *data, int len, uint32_t *out, int maxpx,
                int *w, int *h);
