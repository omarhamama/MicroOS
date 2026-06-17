/*
 * inflate.c — a DEFLATE decompressor, in the spirit of Mark Adler's puff.
 *
 * The bitstream is read least-significant-bit first. A compressed stream
 * is a series of blocks; each block is either stored (raw), or Huffman-
 * coded with either the fixed code tables or per-block "dynamic" tables
 * that the block itself describes. We decode literal/length symbols
 * (0-255 are bytes, 256 ends the block, 257-285 mean "copy a run") and,
 * for a run, a distance symbol saying how far back to copy from. The
 * copy source is the output we've already produced — that's the LZ77
 * sliding window, and we just index back into `out`.
 */

#include "inflate.h"
#include "lib.h"

struct state {
    const uint8_t *in; int inlen, inpos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; int outmax, outpos;
};

/* Canonical-Huffman table: count[len] = #codes of that length;
 * symbol[] lists symbols in canonical (length, then value) order. */
struct huff { short count[16]; short symbol[288]; };

static int bits(struct state *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) return -1;
        val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

/* Decode one symbol from the bitstream using table h. */
static int decode(struct state *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count)               /* code fits this length */
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void build(struct huff *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (short)i;
}

/* length/distance base values and extra-bit counts (RFC 1951 §3.2.5) */
static const short LBASE[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short LEXT[]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DBASE[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short DEXT[]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int codes(struct state *s, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0) return -1;
        if (sym == 256) return 0;               /* end of block */
        if (sym < 256) {                         /* a literal byte */
            if (s->outpos >= s->outmax) return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else {                                 /* a length/distance pair */
            sym -= 257;
            if (sym >= 29) return -1;
            int e = bits(s, LEXT[sym]); if (e < 0) return -1;
            int len = LBASE[sym] + e;
            int dsym = decode(s, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int de = bits(s, DEXT[dsym]); if (de < 0) return -1;
            int distv = DBASE[dsym] + de;
            if (distv > s->outpos) return -1;    /* points before the start */
            if (s->outpos + len > s->outmax) return -1;
            for (int i = 0; i < len; i++) {
                s->out[s->outpos] = s->out[s->outpos - distv];
                s->outpos++;
            }
        }
    }
}

static int dynamic_block(struct state *s)
{
    static const uint8_t order[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit  = bits(s, 5); if (hlit  < 0) return -1; hlit  += 257;
    int hdist = bits(s, 5); if (hdist < 0) return -1; hdist += 1;
    int hclen = bits(s, 4); if (hclen < 0) return -1; hclen += 4;

    uint8_t cl_lengths[19];
    for (int i = 0; i < 19; i++) cl_lengths[i] = 0;
    for (int i = 0; i < hclen; i++) {
        int v = bits(s, 3); if (v < 0) return -1;
        cl_lengths[order[i]] = (uint8_t)v;
    }
    struct huff clh;
    build(&clh, cl_lengths, 19);

    uint8_t lengths[288 + 32];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = decode(s, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {                  /* repeat last 3-6 times */
            if (n == 0) return -1;
            int r = bits(s, 2); if (r < 0) return -1; r += 3;
            uint8_t prev = lengths[n-1];
            while (r-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {                  /* repeat zero 3-10 */
            int r = bits(s, 3); if (r < 0) return -1; r += 3;
            while (r-- && n < total) lengths[n++] = 0;
        } else {                                 /* sym == 18: zero 11-138 */
            int r = bits(s, 7); if (r < 0) return -1; r += 11;
            while (r-- && n < total) lengths[n++] = 0;
        }
    }
    struct huff lit, dist;
    build(&lit, lengths, hlit);
    build(&dist, lengths + hlit, hdist);
    return codes(s, &lit, &dist);
}

static int fixed_block(struct state *s)
{
    uint8_t ll[288], dl[30];
    for (int i = 0; i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0; i < 30; i++) dl[i] = 5;
    struct huff lit, dist;
    build(&lit, ll, 288);
    build(&dist, dl, 30);
    return codes(s, &lit, &dist);
}

static int stored_block(struct state *s)
{
    s->bitbuf = 0; s->bitcnt = 0;                /* discard to byte boundary */
    if (s->inpos + 4 > s->inlen) return -1;
    int len = s->in[s->inpos] | (s->in[s->inpos+1] << 8);
    s->inpos += 4;                               /* skip LEN + NLEN */
    if (s->inpos + len > s->inlen || s->outpos + len > s->outmax) return -1;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len; s->outpos += len;
    return 0;
}

int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outmax)
{
    struct state s = { in, inlen, 0, 0, 0, out, outmax, 0 };
    int final;
    do {
        final = bits(&s, 1);
        int type = bits(&s, 2);
        /* If we ran out of input or hit a bad block, the bytes decoded so
         * far are still a VALID prefix of the stream — return them rather
         * than discarding everything. This is what lets a truncated gzip
         * response (our small TCP can stall on huge pages) still render
         * the part that did arrive. A header error still returns -1. */
        if (final < 0 || type < 0) return s.outpos ? s.outpos : -1;
        int rc;
        if (type == 0)      rc = stored_block(&s);
        else if (type == 1) rc = fixed_block(&s);
        else if (type == 2) rc = dynamic_block(&s);
        else                rc = -1;             /* reserved */
        if (rc < 0) return s.outpos ? s.outpos : -1;
    } while (!final);
    return s.outpos;
}

int gzip_inflate(const uint8_t *in, int inlen, uint8_t *out, int outmax)
{
    /* gzip header: 0x1f 0x8b, method 8, flags, mtime(4), xfl, os = 10 bytes,
     * then optional extra/name/comment fields per the FLG bits. */
    if (inlen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return -1;
    int flg = in[3];
    int p = 10;
    if (flg & 4) {                               /* FEXTRA */
        if (p + 2 > inlen) return -1;
        int xlen = in[p] | (in[p+1] << 8); p += 2 + xlen;
    }
    if (flg & 8)  { while (p < inlen && in[p]) p++; p++; }   /* FNAME */
    if (flg & 16) { while (p < inlen && in[p]) p++; p++; }   /* FCOMMENT */
    if (flg & 2)  p += 2;                        /* FHCRC */
    if (p >= inlen) return -1;
    return inflate_raw(in + p, inlen - p - 8, out, outmax);  /* trailer = 8 */
}

int zlib_inflate(const uint8_t *in, int inlen, uint8_t *out, int outmax)
{
    /* zlib header: CMF, FLG (2 bytes); trailer is a 4-byte adler32. */
    if (inlen < 6 || (in[0] & 0x0f) != 8) return -1;
    return inflate_raw(in + 2, inlen - 2 - 4, out, outmax);
}
