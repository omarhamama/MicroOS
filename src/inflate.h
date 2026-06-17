#pragma once

/*
 * inflate.h — DEFLATE decompression (RFC 1951) and its two wrappers.
 *
 * DEFLATE is the squeeze behind almost everything: HTTP's gzip
 * Content-Encoding, the PNG image format, zip files, git objects. It
 * pairs LZ77 (replace a repeated run with a "copy N bytes from D bytes
 * back" pointer) with Huffman coding (give common bytes short bit codes).
 * Decompressing is the easy direction: read the Huffman trees, then emit
 * literals and replay the back-references.
 *
 * gzip and zlib are the same DEFLATE stream with different few-byte
 * headers/trailers wrapped around it (gzip for HTTP, zlib for PNG).
 */

#include <stdint.h>
#include <stddef.h>

/* All three return the decompressed length, or -1 on malformed input.
 * `out` must be large enough (`outmax`); we don't grow it. */
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outmax);
int gzip_inflate(const uint8_t *in, int inlen, uint8_t *out, int outmax);
int zlib_inflate(const uint8_t *in, int inlen, uint8_t *out, int outmax);
