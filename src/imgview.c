/*
 * imgview.c — decode a picture file and put it on screen.
 *
 * All the hard work lives in png.c and jpeg.c (the same decoders the
 * browser uses for inline images). This file is the small glue: sniff the
 * file's magic bytes to pick a decoder, decode into an RGB buffer, then
 * blit it scaled to fit the window while preserving the aspect ratio
 * (fb_blit_scaled does nearest-neighbour scaling). A second, smaller
 * cached buffer backs the thumbnail shown in the Files preview pane.
 *
 * Decoded images are capped at a few hundred-thousand pixels so a giant
 * photo can't exhaust the 12 MiB heap; oversized or non-baseline files
 * simply report that they can't be shown.
 */

#include "imgview.h"
#include "png.h"
#include "jpeg.h"
#include "fs.h"
#include "fb.h"
#include "mem.h"
#include "lib.h"
#include "kprintf.h"

#define IV_MAXPX 600000             /* ~775x775 ceiling for the main view */
#define TH_MAXPX 200000             /* ~450x450 ceiling for a thumbnail   */

/* Decode by magic: 0x89 'P' = PNG, 0xFF 0xD8 = JPEG. 0 ok, -1 otherwise. */
static int img_decode(const uint8_t *d, uint32_t len, uint32_t *out, int maxpx,
                      int *w, int *h)
{
    if (len > 8 && d[0] == 0x89 && d[1] == 'P')
        return png_decode(d, (int)len, out, maxpx, w, h);
    if (len > 3 && d[0] == 0xFF && d[1] == 0xD8)
        return jpeg_decode(d, (int)len, out, maxpx, w, h);
    return -1;
}

static int ends_ci(const char *s, const char *suf)
{
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    if (ls < lf) return 0;
    for (int i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return 0;
    }
    return 1;
}

int img_is_image(const char *name)
{
    return ends_ci(name, ".png") || ends_ci(name, ".jpg") ||
           ends_ci(name, ".jpeg") || ends_ci(name, ".jpe");
}

/* Blit `px` (sw×sh) into the (x,y,w,h) box, centered, aspect-preserved. */
static void blit_fit(int x, int y, int w, int h, const uint32_t *px, int sw, int sh)
{
    int dw = w, dh = (int)((int64_t)w * sh / sw);
    if (dh > h) { dh = h; dw = (int)((int64_t)h * sw / sh); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    fb_blit_scaled(x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, px, sw, sh);
}

/* ---- the main viewer ------------------------------------------------ */

static uint32_t *iv_px;
static int       iv_w, iv_h;
static char      iv_name[FS_NAME_MAX];
static char      iv_err[56];

const char *imgview_name(void) { return iv_name; }

void imgview_open(struct fs_node *n)
{
    iv_err[0] = '\0';
    iv_w = iv_h = 0;
    int i = 0;
    if (n) for (; n->name[i] && i < FS_NAME_MAX - 1; i++) iv_name[i] = n->name[i];
    iv_name[i] = '\0';

    if (!iv_px) iv_px = kmalloc((uint64_t)IV_MAXPX * 4);
    if (!iv_px) { ksprintf(iv_err, sizeof iv_err, "Out of memory"); return; }
    if (!n || !n->data || n->size < 4) {
        ksprintf(iv_err, sizeof iv_err, "Empty file");
        return;
    }
    int w, h;
    if (img_decode((const uint8_t *)n->data, (uint32_t)n->size, iv_px,
                   IV_MAXPX, &w, &h) == 0) {
        iv_w = w; iv_h = h;
    } else {
        ksprintf(iv_err, sizeof iv_err,
                 "Can't display (not PNG/baseline JPEG, or too large)");
    }
}

void imgview_draw(int x, int y, int w, int h)
{
    fb_rect(x, y, w, h, 0x101014);          /* dark mat behind the picture */
    if (iv_w > 0 && iv_px) {
        blit_fit(x, y, w, h, iv_px, iv_w, iv_h);
    } else {
        const char *m = iv_err[0] ? iv_err : "No image open";
        fb_text(x + (w - fb_text_width(m, 1)) / 2, y + h / 2 - 4, m, 0xc0c0c6, 1);
    }
}

/* ---- the Files thumbnail (decodes only when the selection changes) -- */

static struct fs_node *th_node;
static uint32_t       *th_px;
static int             th_w, th_h, th_ok;

int imgview_thumb(struct fs_node *n, int x, int y, int maxw, int maxh)
{
    if (!n) return 0;
    if (n != th_node) {                     /* selection changed: re-decode */
        th_node = n; th_ok = 0; th_w = th_h = 0;
        if (n->data && n->size >= 4 && img_is_image(n->name)) {
            if (!th_px) th_px = kmalloc((uint64_t)TH_MAXPX * 4);
            int w, h;
            if (th_px && img_decode((const uint8_t *)n->data, (uint32_t)n->size,
                                    th_px, TH_MAXPX, &w, &h) == 0) {
                th_w = w; th_h = h; th_ok = 1;
            }
        }
    }
    if (!th_ok) return 0;
    blit_fit(x, y, maxw, maxh, th_px, th_w, th_h);
    return 1;
}
