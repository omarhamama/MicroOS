/*
 * png.c — a PNG decoder (8-bit, non-interlaced).
 */

#include "png.h"
#include "inflate.h"
#include "mem.h"
#include "lib.h"

static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static int iabs(int x){ return x<0?-x:x; }

/* The Paeth predictor: pick whichever of left/up/up-left the gradient
 * a+b-c is closest to. (PNG's best general-purpose filter.) */
static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = iabs(p-a), pb = iabs(p-b), pc = iabs(p-c);
    if (pa<=pb && pa<=pc) return a;
    if (pb<=pc) return b;
    return c;
}

int png_decode(const uint8_t *data, int len, uint32_t *out, int maxpx,
               int *ow, int *oh)
{
    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    if (len < 8 || memcmp(data, sig, 8) != 0) return -1;

    int w=0, h=0, depth=0, ctype=0, interlace=0;
    uint8_t plte[256*3]; int nplte=0;

    /* IDAT can span many chunks — concatenate into one buffer. */
    uint8_t *idat = kmalloc(len);       /* compressed never exceeds file size */
    if (!idat) return -1;
    int idat_len = 0;
    int rc = -1;

    int p = 8;
    while (p + 8 <= len) {
        uint32_t clen = be32(data+p);
        const uint8_t *type = data+p+4;
        const uint8_t *cdat = data+p+8;
        if (p + 12 + (int)clen > len) break;     /* +12 = len+type+crc */

        if (!memcmp(type,"IHDR",4) && clen>=13) {
            w = (int)be32(cdat); h = (int)be32(cdat+4);
            depth = cdat[8]; ctype = cdat[9]; interlace = cdat[12];
        } else if (!memcmp(type,"PLTE",4)) {
            nplte = clen/3; if (nplte>256) nplte=256;
            memcpy(plte, cdat, nplte*3);
        } else if (!memcmp(type,"IDAT",4)) {
            if (idat_len + (int)clen <= len) { memcpy(idat+idat_len, cdat, clen); idat_len += clen; }
        } else if (!memcmp(type,"IEND",4)) {
            break;
        }
        p += 12 + clen;
    }

    if (w<=0 || h<=0 || depth!=8 || interlace!=0) goto done;
    int ch;                                       /* samples per pixel */
    switch (ctype) { case 0: ch=1; break; case 2: ch=3; break;
                     case 3: ch=1; break; case 4: ch=2; break;
                     case 6: ch=4; break; default: goto done; }
    if ((int64_t)w*h > maxpx) goto done;

    int stride = w*ch;
    int rawmax = h*(stride+1);                    /* +1 filter byte per row */
    uint8_t *raw = kmalloc(rawmax);
    if (!raw) goto done;
    int got = zlib_inflate(idat, idat_len, raw, rawmax);
    if (got < h*(stride+1)) { kfree(raw); goto done; }

    /* un-filter, row by row, in place within raw's pixel area */
    for (int y=0; y<h; y++) {
        uint8_t *row = raw + y*(stride+1);
        int filt = row[0];
        uint8_t *cur = row+1;
        uint8_t *prev = y? raw + (y-1)*(stride+1) + 1 : 0;
        for (int i=0;i<stride;i++) {
            int a = i>=ch ? cur[i-ch] : 0;
            int b = prev ? prev[i] : 0;
            int cc = (prev && i>=ch) ? prev[i-ch] : 0;
            int v = cur[i];
            switch (filt) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a+b)/2; break;
            case 4: v += paeth(a,b,cc); break;
            }
            cur[i] = (uint8_t)v;
        }
    }

    /* convert to 0xRRGGBB, compositing alpha over white */
    for (int y=0;y<h;y++) {
        uint8_t *cur = raw + y*(stride+1) + 1;
        for (int x=0;x<w;x++) {
            uint8_t *s = cur + x*ch;
            uint32_t r,g,b,a=255;
            if (ctype==0) { r=g=b=s[0]; }
            else if (ctype==4) { r=g=b=s[0]; a=s[1]; }
            else if (ctype==2) { r=s[0]; g=s[1]; b=s[2]; }
            else if (ctype==6) { r=s[0]; g=s[1]; b=s[2]; a=s[3]; }
            else { int idx=s[0]; r=plte[idx*3]; g=plte[idx*3+1]; b=plte[idx*3+2]; }
            if (a != 255) {                       /* over white background */
                r = (r*a + 255*(255-a))/255;
                g = (g*a + 255*(255-a))/255;
                b = (b*a + 255*(255-a))/255;
            }
            out[y*w+x] = (r<<16)|(g<<8)|b;
        }
    }
    *ow = w; *oh = h; rc = 0;
    kfree(raw);
done:
    kfree(idat);
    return rc;
}
