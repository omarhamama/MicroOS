#pragma once

#include <stdint.h>
#include "board.h"

/* Screen size comes from the board (1024x768 on QEMU/Pi, 640x480 on the
 * R36S). The whole GUI is laid out relative to these. */
#define FB_WIDTH   BOARD_FB_W
#define FB_HEIGHT  BOARD_FB_H

/* Colors are 0xRRGGBB (the framebuffer format is XRGB8888).
 * All drawing goes to a BACK buffer; fb_flip() shows the finished
 * frame in one copy (double buffering — no torn frames). */
int  fb_init(void);                 /* 0 = display live, -1 = no framebuffer */
int  fb_present(void);
void fb_set_simplefb(uint64_t addr, int pitch);   /* R36S: bootloader's FB */
uint32_t *fb_pixels(void);          /* the back buffer */
void fb_flip(void);

void fb_fill(uint32_t color);
void fb_rect(int x, int y, int w, int h, uint32_t color);
void fb_text(int x, int y, const char *s, uint32_t fg, int scale);
int  fb_text_width(const char *s, int scale);
void fb_set_font(const unsigned char (*glyphs)[8]);   /* swap the system font */
void fb_text_with(int x, int y, const char *s, uint32_t fg, int scale,
                  const unsigned char (*glyphs)[8]);   /* draw in one font */

/* The modern-UI toolkit: translucency and curves. */
void fb_blend_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
void fb_circle(int cx, int cy, int r, uint32_t color);
void fb_rounded(int x, int y, int w, int h, int r, uint32_t color);
void fb_rounded_blend(int x, int y, int w, int h, int r,
                      uint32_t color, uint8_t alpha);

/* Blit a sw*sh pixel image (0xRRGGBB, row-major) into the dw*dh
 * rectangle at (dx,dy), nearest-neighbour scaled and screen-clipped. */
void fb_blit_scaled(int dx, int dy, int dw, int dh,
                    const uint32_t *src, int sw, int sh);

/* Draw a 16x16 1-bit glyph (16 rows of 16 bits, LSB = leftmost pixel) at
 * (x,y) in `color`. Used for the Arabic + proportional bitmap fonts. */
void fb_glyph16(int x, int y, const uint16_t rows[16], uint32_t color);
void fb_glyph16_scaled(int x, int y, const uint16_t rows[16], uint32_t color, int scale);
