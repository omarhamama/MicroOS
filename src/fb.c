/*
 * fb.c — the framebuffer: a screen is just an array.
 *
 * After all the abstraction in this project, the display is almost
 * anticlimactic: the screen is RAM. One uint32_t per pixel (0xRRGGBB),
 * row after row. Light a pixel = write to an array. Draw a letter =
 * copy 64 bits of font bitmap into the right spots. Everything on
 * every screen you've ever seen bottoms out in a loop like fb_rect.
 *
 * The display device is QEMU's "ramfb", the simplest one possible: no
 * GPU, no command queue — we tell it (via fw_cfg, see fwcfg.c) "the
 * framebuffer lives at this address, 800x600, XRGB8888" and it just
 * scans our memory out to the window ~60 times a second. Whatever the
 * array holds at that instant is what you see.
 *
 * Real GPUs differ in performance, not principle: same framebuffer at
 * the end, plus dedicated hardware to fill it faster than a CPU loop.
 */

#include "fb.h"
#include "fwcfg.h"
#include "font.h"
#include "mem.h"
#include "lib.h"
#include "board.h"
#include "mailbox.h"

/*
 * Two buffers, one lesson: QEMU scans the FRONT buffer out to the
 * window ~60 times a second, at moments we don't control. Draw
 * directly into it and the viewer sees half-finished frames — the
 * background painted but the windows not yet ("tearing"/flicker).
 * So we compose each frame off-screen in the BACK buffer and reveal
 * it with one fast copy: fb_flip(). Double buffering — same reason
 * every toolkit and game engine does it.
 */
static uint32_t *fb;                /* front: what the display scans out */
static uint32_t *back;              /* back: what drawing writes to */
static int fb_pitch = FB_WIDTH * 4; /* scanout row stride (bytes) */

#if BOARD_FB_SIMPLEFB
/* The R36S reuses the framebuffer U-Boot already set up; kernel.c finds
 * it in the DTB and hands us the address + row stride before fb_init. */
static uint64_t simplefb_addr;
static int      simplefb_pitch;
void fb_set_simplefb(uint64_t addr, int pitch)
{
    simplefb_addr = addr;
    simplefb_pitch = pitch;
}
#endif

#if !BOARD_FB_MAILBOX && !BOARD_FB_SIMPLEFB
/* The struct QEMU expects in the "etc/ramfb" fw_cfg file — all fields
 * BIG-endian. fourcc picks the pixel format; "XR24" = XRGB8888. */
struct ramfb_cfg {
    uint64_t addr;
    uint32_t fourcc, flags, width, height, stride;
} __attribute__((packed));

static uint32_t be32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
static uint64_t be64(uint64_t v)
{
    return ((uint64_t)be32((uint32_t)v) << 32) | be32((uint32_t)(v >> 32));
}
#endif

int fb_present(void)      { return fb != 0; }
uint32_t *fb_pixels(void) { return back; }

void fb_flip(void)
{
    if (fb_pitch == FB_WIDTH * 4) {             /* tight: one fast copy */
        memcpy(fb, back, FB_WIDTH * FB_HEIGHT * 4);
    } else {                                    /* padded rows (Pi may pad) */
        for (int y = 0; y < FB_HEIGHT; y++)
            memcpy((uint8_t *)fb + (uint64_t)y * fb_pitch,
                   back + (uint64_t)y * FB_WIDTH, FB_WIDTH * 4);
    }
}

#if BOARD_FB_MAILBOX
/* Raspberry Pi: the framebuffer comes from the VideoCore GPU. We scan out
 * of the GPU's buffer and keep our own back-buffer in RAM. */
int fb_init(void)
{
    int pitch = FB_WIDTH * 4;
    uint32_t *hw = mailbox_fb_init(FB_WIDTH, FB_HEIGHT, &pitch);
    if (!hw)
        return -1;
    back = kmalloc(FB_WIDTH * FB_HEIGHT * 4);
    if (!back)
        return -1;
    fb = hw;
    fb_pitch = pitch ? pitch : FB_WIDTH * 4;
    memset(back, 0, FB_WIDTH * FB_HEIGHT * 4);
    return 0;
}
#elif BOARD_FB_SIMPLEFB
/* R36S / RK3326: draw into the framebuffer U-Boot left running. */
int fb_init(void)
{
    if (!simplefb_addr)
        return -1;                  /* no simple-framebuffer in the DTB */
    back = kmalloc(FB_WIDTH * FB_HEIGHT * 4);
    if (!back)
        return -1;
    fb = (uint32_t *)(uintptr_t)simplefb_addr;
    fb_pitch = simplefb_pitch ? simplefb_pitch : FB_WIDTH * 4;
    memset(back, 0, FB_WIDTH * FB_HEIGHT * 4);
    return 0;
}
#else
int fb_init(void)
{
    uint16_t sel = fwcfg_find("etc/ramfb");
    if (!sel)
        return -1;                  /* QEMU started without -device ramfb */

    fb = kmalloc(FB_WIDTH * FB_HEIGHT * 4);
    back = kmalloc(FB_WIDTH * FB_HEIGHT * 4);
    if (!fb || !back)
        return -1;
    memset(fb, 0, FB_WIDTH * FB_HEIGHT * 4);
    memset(back, 0, FB_WIDTH * FB_HEIGHT * 4);

    struct ramfb_cfg cfg = {
        .addr   = be64((uint64_t)fb),
        .fourcc = be32(0x34325258),         /* 'XR24' little-endian */
        .flags  = 0,
        .width  = be32(FB_WIDTH),
        .height = be32(FB_HEIGHT),
        .stride = be32(FB_WIDTH * 4),
    };
    if (fwcfg_dma_write(sel, &cfg, sizeof(cfg)) < 0) {
        kfree(fb);
        fb = 0;
        return -1;
    }
    return 0;
}
#endif

void fb_fill(uint32_t color)
{
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++)
        back[i] = color;
}

void fb_rect(int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = back + (uint64_t)(y + j) * FB_WIDTH + x;
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
}

/*
 * The two techniques that separate a "90s" UI from a modern one:
 *
 * ALPHA BLENDING — instead of overwriting a pixel, MIX with what's
 * already there: out = src*a + dst*(1-a). That one formula is
 * translucent menu bars, soft shadows, frosted docks. (Real
 * compositors do it in hardware; the math is identical.)
 *
 * ROUNDED CORNERS — a corner is a quarter circle: a row `dy` from the
 * corner's center starts `r - sqrt(r^2 - dy^2)` pixels in. We find
 * that inset with an integer scan (r is tiny; no FPU needed, which is
 * good because the kernel compiles without one).
 */

static void blend_span(uint32_t *row, int n, uint32_t color, uint8_t alpha)
{
    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF,
             sb = color & 0xFF;
    for (int i = 0; i < n; i++) {
        uint32_t d = row[i];
        uint32_t r = (sr * alpha + ((d >> 16) & 0xFF) * (255u - alpha)) / 255u;
        uint32_t g = (sg * alpha + ((d >> 8) & 0xFF) * (255u - alpha)) / 255u;
        uint32_t b = (sb * alpha + (d & 0xFF) * (255u - alpha)) / 255u;
        row[i] = (r << 16) | (g << 8) | b;
    }
}

void fb_blend_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    for (int j = 0; j < h; j++)
        blend_span(back + (uint64_t)(y + j) * FB_WIDTH + x, w, color, alpha);
}

static int corner_inset(int r, int dy)
{
    for (int dx = 0; dx <= r; dx++)
        if (dx * dx + dy * dy >= r * r)
            return r - dx;
    return 0;
}

/* One row-span walk serves both the solid and translucent variants. */
static void rounded_rows(int x, int y, int w, int h, int r,
                         uint32_t color, int alpha)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r)
            inset = corner_inset(r, r - j);
        else if (j >= h - r)
            inset = corner_inset(r, j - (h - r - 1));

        int rx = x + inset, rw = w - 2 * inset, ry = y + j;
        if (ry < 0 || ry >= FB_HEIGHT)
            continue;
        if (rx < 0) { rw += rx; rx = 0; }
        if (rx + rw > FB_WIDTH)
            rw = FB_WIDTH - rx;
        if (rw <= 0)
            continue;

        uint32_t *row = back + (uint64_t)ry * FB_WIDTH + rx;
        if (alpha < 0)
            for (int i = 0; i < rw; i++)
                row[i] = color;
        else
            blend_span(row, rw, color, (uint8_t)alpha);
    }
}

void fb_rounded(int x, int y, int w, int h, int r, uint32_t color)
{
    rounded_rows(x, y, w, h, r, color, -1);
}

void fb_rounded_blend(int x, int y, int w, int h, int r,
                      uint32_t color, uint8_t alpha)
{
    rounded_rows(x, y, w, h, r, color, alpha);
}

void fb_glyph16(int x, int y, const uint16_t rows[16], uint32_t color)
{
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || py >= FB_HEIGHT) continue;
        uint16_t bits = rows[r];
        if (!bits) continue;
        uint32_t *row = back + (uint64_t)py * FB_WIDTH;
        for (int b = 0; b < 16; b++)
            if (bits & (1u << b)) {              /* LSB = leftmost */
                int px = x + b;
                if (px >= 0 && px < FB_WIDTH) row[px] = color;
            }
    }
}

/* Like fb_glyph16 but each font pixel becomes a scale×scale block — for
 * larger text (headings) without a second font. */
void fb_glyph16_scaled(int x, int y, const uint16_t rows[16], uint32_t color, int scale)
{
    if (scale <= 1) { fb_glyph16(x, y, rows, color); return; }
    for (int r = 0; r < 16; r++) {
        uint16_t bits = rows[r];
        if (!bits) continue;
        for (int b = 0; b < 16; b++)
            if (bits & (1u << b))
                fb_rect(x + b * scale, y + r * scale, scale, scale, color);
    }
}

void fb_circle(int cx, int cy, int r, uint32_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int half = 0;
        while (half * half + dy * dy <= r * r)
            half++;
        fb_rect(cx - half + 1, cy + dy, 2 * half - 1, 1, color);
    }
}

void fb_blit_scaled(int dx, int dy, int dw, int dh,
                    const uint32_t *src, int sw, int sh)
{
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    for (int j = 0; j < dh; j++) {
        int y = dy + j;
        if (y < 0 || y >= FB_HEIGHT) continue;
        const uint32_t *srow = src + (uint64_t)(j * sh / dh) * sw;
        uint32_t *drow = back + (uint64_t)y * FB_WIDTH;
        for (int i = 0; i < dw; i++) {
            int x = dx + i;
            if (x < 0 || x >= FB_WIDTH) continue;
            drow[x] = srow[i * sw / dw];        /* nearest-neighbour sample */
        }
    }
}

/* The ACTIVE system font — a pointer the font manager can repoint, so
 * one assignment re-skins the whole desktop on the next repaint. It
 * starts as the built-in 8x8 font (fb.c works before fonts.c runs). */
static const unsigned char (*g_font)[8] = font8x8;

void fb_set_font(const unsigned char (*glyphs)[8])
{
    g_font = glyphs ? glyphs : font8x8;
}

/* A character: 8 font bytes, one per row, LSB = leftmost pixel.
 * `scale` doubles/triples each font pixel into a block. Writes pixels
 * directly — the console window draws thousands of glyphs per frame,
 * and routing every font pixel through fb_rect's clipping was a third
 * of the whole frame budget. */
static void fb_char(int x, int y, char c, uint32_t fg, int scale)
{
    const unsigned char *glyph = g_font[(unsigned char)c & 0x7F];

    if (x < 0 || y < 0 || x + 8 * scale > FB_WIDTH || y + 8 * scale > FB_HEIGHT)
        return;                             /* clip whole glyphs, once */

    for (int row = 0; row < 8; row++) {
        unsigned bits = glyph[row];
        if (!bits)
            continue;                       /* blank rows cost nothing */
        for (int sy = 0; sy < scale; sy++) {
            uint32_t *p = back + (uint64_t)(y + row * scale + sy) * FB_WIDTH + x;
            for (int col = 0; col < 8; col++)
                if (bits & (1u << col))
                    for (int sx = 0; sx < scale; sx++)
                        p[col * scale + sx] = fg;
        }
    }
}

/* Draw a string in a SPECIFIC font (for the font manager's previews),
 * leaving the system font unchanged. */
void fb_text_with(int x, int y, const char *s, uint32_t fg, int scale,
                  const unsigned char (*glyphs)[8])
{
    const unsigned char (*save)[8] = g_font;
    g_font = glyphs ? glyphs : g_font;
    fb_text(x, y, s, fg, scale);
    g_font = save;
}

void fb_text(int x, int y, const char *s, uint32_t fg, int scale)
{
    for (; *s; s++) {
        if (x + 8 * scale > FB_WIDTH)
            break;
        fb_char(x, y, *s, fg, scale);
        x += 8 * scale;
    }
}

int fb_text_width(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}
