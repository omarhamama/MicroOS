/*
 * jpeg.c — a baseline JPEG decoder.
 *
 * The file is a stream of marker segments: DQT (quantisation tables),
 * DHT (Huffman tables), SOF0 (image size + components + chroma sampling),
 * SOS (start of the entropy-coded scan), with DRI/RSTn for restart
 * intervals. After SOS comes the bitstream: for each 8x8 block we
 * Huffman-decode a DC coefficient (stored as a difference from the
 * previous block) and a run-length list of AC coefficients, de-quantise,
 * de-zigzag, inverse-DCT to pixels, and (for colour) convert YCbCr->RGB,
 * upsampling the chroma planes that subsampling shrank.
 */

#include "jpeg.h"
#include "mem.h"
#include "lib.h"

/* Fixed-point inverse-DCT cosine table: C[k][n] = Λ(k)·cos((2n+1)kπ/16)
 * × 1024  (Λ(0)=1/√2). Precomputed because the kernel has no FPU. */
static const int IDCT_C[8][8] = {
    {  724,  724,  724,  724,  724,  724,  724,  724},
    { 1004,  851,  569,  200, -200, -569, -851,-1004},
    {  946,  392, -392, -946, -946, -392,  392,  946},
    {  851, -200,-1004, -569,  569, 1004,  200, -851},
    {  724, -724, -724,  724,  724, -724, -724,  724},
    {  569,-1004,  200,  851, -851, -200, 1004, -569},
    {  392, -946,  946, -392, -392,  946, -946,  392},
    {  200, -569,  851,-1004, 1004, -851,  569, -200},
};

static const uint8_t ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

/* JPEG canonical Huffman table (per JPEG Annex C/F). */
struct huff {
    uint8_t vals[256];
    int     mincode[17], maxcode[18], valptr[17];
    int     present;
};

struct comp {
    int id, hs, vs, qt;     /* sampling factors + quant-table id */
    int dc_pred;
    int dc_tbl, ac_tbl;
};

struct jpeg {
    const uint8_t *d; int len, pos;
    uint32_t bitbuf; int bitcnt; int hit_marker;
    uint16_t quant[4][64];
    struct huff dc[4], ac[4];
    struct comp comp[4]; int ncomp;
    int w, h, maxh, maxv;
    int restart;
};

/* ---- bit reader (handles 0xFF byte-stuffing) ---------------------- */
static int next_byte(struct jpeg *j)
{
    if (j->pos >= j->len) return -1;
    uint8_t b = j->d[j->pos++];
    if (b == 0xFF) {
        while (j->pos < j->len && j->d[j->pos] == 0xFF) j->pos++;  /* fill bytes */
        uint8_t m = (j->pos < j->len) ? j->d[j->pos] : 0xD9;
        if (m == 0x00) { j->pos++; return 0xFF; }     /* stuffed literal 0xFF */
        j->hit_marker = m;                            /* a real marker: stop */
        return -1;
    }
    return b;
}
static int getbit(struct jpeg *j)
{
    if (j->bitcnt == 0) {
        int b = next_byte(j);
        if (b < 0) return 0;                          /* past end: feed zeros */
        j->bitbuf = (uint32_t)b; j->bitcnt = 8;
    }
    j->bitcnt--;
    return (j->bitbuf >> j->bitcnt) & 1;
}
static int getbits(struct jpeg *j, int n)
{
    int v = 0;
    while (n--) v = (v << 1) | getbit(j);
    return v;
}
/* sign-extend an n-bit JPEG "receive" value */
static int extend(int v, int n)
{
    return (v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

static int huff_decode(struct jpeg *j, struct huff *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | getbit(j);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l])
            return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return 0;
}

/* Build mincode/maxcode/valptr from the 16 length-counts. */
static void build_huff(struct huff *h, const uint8_t counts[16])
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        if (counts[l-1] == 0) { h->maxcode[l] = -1; }
        else {
            h->valptr[l] = k;
            h->mincode[l] = code;
            code += counts[l-1]; k += counts[l-1];
            h->maxcode[l] = code - 1;
        }
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    h->present = 1;
}

/* ---- inverse DCT: integer, separable (rows then columns) ---------- */
static void idct(const int blk[64], uint8_t *out, int stride)
{
    int64_t tmp[64];
    for (int r = 0; r < 8; r++)                  /* pass 1: each row */
        for (int x = 0; x < 8; x++) {
            int64_t s = 0;
            for (int c = 0; c < 8; c++) s += (int64_t)blk[r*8+c] * IDCT_C[c][x];
            tmp[r*8+x] = s;
        }
    for (int x = 0; x < 8; x++)                  /* pass 2: each column */
        for (int y = 0; y < 8; y++) {
            int64_t s = 0;
            for (int r = 0; r < 8; r++) s += IDCT_C[r][y] * tmp[r*8+x];
            int v = (int)((s + (1<<21)) >> 22) + 128;   /* descale + level shift */
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            out[y*stride + x] = (uint8_t)v;
        }
}

/* ---- marker parsing ----------------------------------------------- */
static int rd16(struct jpeg *j) { int v = (j->d[j->pos]<<8)|j->d[j->pos+1]; j->pos += 2; return v; }

int jpeg_decode(const uint8_t *data, int len, uint32_t *out, int maxpx,
                int *ow, int *oh)
{
    struct jpeg *j = kmalloc(sizeof *j);
    if (!j) return -1;
    memset(j, 0, sizeof *j);
    j->d = data; j->len = len; j->pos = 0;

    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) { kfree(j); return -1; } /* SOI */
    j->pos = 2;

    uint8_t *plane[4] = {0,0,0,0};
    int pl_w[4] = {0}, pl_h[4] = {0};
    int rc = -1, sof_seen = 0;

    while (j->pos + 4 <= len) {
        if (data[j->pos] != 0xFF) { j->pos++; continue; }
        int m = data[j->pos+1]; j->pos += 2;
        if (m == 0xD9) break;                         /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;  /* standalone */
        int seglen = rd16(j);
        int seg_end = j->pos + seglen - 2;
        if (seg_end > len) break;

        if (m == 0xDB) {                              /* DQT */
            while (j->pos < seg_end) {
                int pq = data[j->pos] >> 4, tq = data[j->pos] & 15; j->pos++;
                if (tq > 3) goto done;
                for (int i = 0; i < 64; i++)
                    j->quant[tq][i] = pq ? rd16(j) : data[j->pos++];
            }
        } else if (m == 0xC0 || m == 0xC1) {          /* SOF0/1: baseline */
            j->pos++;                                  /* precision */
            j->h = rd16(j); j->w = rd16(j);
            j->ncomp = data[j->pos++];
            if (j->ncomp > 4 || j->w <= 0 || j->h <= 0) goto done;
            if ((int64_t)j->w * j->h > maxpx) goto done;
            j->maxh = j->maxv = 1;
            for (int c = 0; c < j->ncomp; c++) {
                j->comp[c].id = data[j->pos++];
                j->comp[c].hs = data[j->pos] >> 4;
                j->comp[c].vs = data[j->pos] & 15; j->pos++;
                j->comp[c].qt = data[j->pos++];
                if (j->comp[c].hs > j->maxh) j->maxh = j->comp[c].hs;
                if (j->comp[c].vs > j->maxv) j->maxv = j->comp[c].vs;
            }
            sof_seen = 1;
        } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF)) {
            goto done;                                /* progressive/other: unsupported */
        } else if (m == 0xC4) {                       /* DHT */
            while (j->pos < seg_end) {
                int tc = data[j->pos] >> 4, th = data[j->pos] & 15; j->pos++;
                if (th > 3) goto done;
                uint8_t counts[16]; int total = 0;
                for (int i = 0; i < 16; i++) { counts[i] = data[j->pos++]; total += counts[i]; }
                struct huff *h = tc ? &j->ac[th] : &j->dc[th];
                build_huff(h, counts);
                for (int i = 0; i < total && i < 256; i++) h->vals[i] = data[j->pos++];
            }
        } else if (m == 0xDD) {                       /* DRI: restart interval */
            j->restart = rd16(j);
        } else if (m == 0xDA) {                       /* SOS: the scan */
            if (!sof_seen) goto done;
            int ns = data[j->pos++];
            for (int s = 0; s < ns; s++) {
                int cid = data[j->pos++], t = data[j->pos++];
                for (int c = 0; c < j->ncomp; c++)
                    if (j->comp[c].id == cid) { j->comp[c].dc_tbl = t>>4; j->comp[c].ac_tbl = t&15; }
            }
            j->pos += 3;                              /* Ss, Se, Ah/Al */
            goto scan;
        } else {
            j->pos = seg_end;                         /* APPn/COM/etc: skip */
        }
        if (j->pos < seg_end) j->pos = seg_end;
        continue;

scan:;
        /* ---- decode the entropy-coded scan ---- */
        int mcuw = 8 * j->maxh, mcuh = 8 * j->maxv;
        int mcusx = (j->w + mcuw - 1) / mcuw;
        int mcusy = (j->h + mcuh - 1) / mcuh;
        for (int c = 0; c < j->ncomp; c++) {
            pl_w[c] = mcusx * j->comp[c].hs * 8;
            pl_h[c] = mcusy * j->comp[c].vs * 8;
            plane[c] = kmalloc((uint64_t)pl_w[c] * pl_h[c]);
            if (!plane[c]) goto done;
            j->comp[c].dc_pred = 0;
        }
        j->bitcnt = 0; j->hit_marker = 0;
        int rst_count = 0;

        for (int my = 0; my < mcusy; my++) {
            for (int mx = 0; mx < mcusx; mx++) {
                /* restart interval: realign + reset DC predictors */
                if (j->restart && rst_count == j->restart) {
                    j->bitcnt = 0;
                    /* skip to and past the RSTn marker */
                    while (j->pos + 1 < len && !(data[j->pos]==0xFF && data[j->pos+1]>=0xD0
                                                 && data[j->pos+1]<=0xD7)) j->pos++;
                    if (j->pos + 1 < len) j->pos += 2;
                    j->hit_marker = 0; rst_count = 0;
                    for (int c = 0; c < j->ncomp; c++) j->comp[c].dc_pred = 0;
                }
                for (int c = 0; c < j->ncomp; c++) {
                    struct comp *cm = &j->comp[c];
                    for (int by = 0; by < cm->vs; by++)
                    for (int bx = 0; bx < cm->hs; bx++) {
                        int blk[64]; memset(blk, 0, sizeof blk);
                        /* DC */
                        int s = huff_decode(j, &j->dc[cm->dc_tbl]);
                        int diff = s ? extend(getbits(j, s), s) : 0;
                        cm->dc_pred += diff;
                        blk[0] = cm->dc_pred * j->quant[cm->qt][0];
                        /* AC */
                        for (int k = 1; k < 64; ) {
                            int rs = huff_decode(j, &j->ac[cm->ac_tbl]);
                            int r = rs >> 4, sz = rs & 15;
                            if (sz == 0) { if (r == 15) { k += 16; continue; } break; }
                            k += r;
                            if (k > 63) break;
                            int val = extend(getbits(j, sz), sz);
                            blk[ZIGZAG[k]] = val * j->quant[cm->qt][k];
                            k++;
                        }
                        int px = (mx * cm->hs + bx) * 8;
                        int py = (my * cm->vs + by) * 8;
                        idct(blk, plane[c] + py * pl_w[c] + px, pl_w[c]);
                    }
                }
                rst_count++;
            }
        }

        /* ---- compose pixels (upsample chroma + YCbCr->RGB) ---- */
        for (int y = 0; y < j->h; y++) {
            for (int x = 0; x < j->w; x++) {
                int Y, Cb = 128, Cr = 128;
                int yx = x * j->comp[0].hs / j->maxh;
                int yy = y * j->comp[0].vs / j->maxv;
                Y = plane[0][yy * pl_w[0] + yx];
                if (j->ncomp == 3) {
                    int bx = x * j->comp[1].hs / j->maxh, by = y * j->comp[1].vs / j->maxv;
                    int rx = x * j->comp[2].hs / j->maxh, ry = y * j->comp[2].vs / j->maxv;
                    Cb = plane[1][by * pl_w[1] + bx];
                    Cr = plane[2][ry * pl_w[2] + rx];
                }
                int R = Y + ((91881  * (Cr-128)) >> 16);
                int G = Y - ((22554  * (Cb-128) + 46802 * (Cr-128)) >> 16);
                int B = Y + ((116130 * (Cb-128)) >> 16);
                R = R<0 ? 0 : R>255 ? 255 : R;
                G = G<0 ? 0 : G>255 ? 255 : G;
                B = B<0 ? 0 : B>255 ? 255 : B;
                out[y * j->w + x] = (R<<16)|(G<<8)|B;
            }
        }
        *ow = j->w; *oh = j->h; rc = 0;
        break;                                        /* one scan = done (baseline) */
    }

done:
    for (int c = 0; c < 4; c++) if (plane[c]) kfree(plane[c]);
    kfree(j);
    return rc;
}
