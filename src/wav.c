/*
 * wav.c — parse a RIFF/WAVE file and stream it as 44.1k stereo PCM.
 *
 * A WAV is a list of "chunks", each = 4-byte tag + 4-byte little-endian
 * length + that many bytes. We need two: "fmt " (how the samples are
 * encoded) and "data" (the samples). Everything else we skip.
 *
 * Then the only real work is RESAMPLING: the file might be 22050 Hz mono,
 * the card wants 44100 Hz stereo. We walk a fractional cursor through the
 * source at rate/44100 frames per output frame (nearest-sample — crude
 * but clean), duplicating mono to both channels. All fixed-point: the
 * cursor is Q16 (low 16 bits are the fraction).
 */

#include "wav.h"

#define RATE 44100u

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

int wav_open(struct wav *w, const uint8_t *file, uint32_t len)
{
    if (len < 44 || rd32(file) != 0x46464952u /* "RIFF" */ ||
        rd32(file + 8) != 0x45564157u /* "WAVE" */)
        return -1;

    int have_fmt = 0;
    w->data = file;
    w->len = len;
    w->data_off = w->data_len = 0;

    uint32_t off = 12;
    while (off + 8 <= len) {
        uint32_t id   = rd32(file + off);
        uint32_t clen = rd32(file + off + 4);
        const uint8_t *body = file + off + 8;
        if (off + 8 + clen > len)
            clen = len - off - 8;           /* truncated file: take what's there */

        if (id == 0x20746d66u /* "fmt " */ && clen >= 16) {
            uint32_t fmt = rd16(body);      /* 1 = PCM */
            w->channels  = (int)rd16(body + 2);
            w->rate      = rd32(body + 4);
            w->bits      = (int)rd16(body + 14);
            if (fmt != 1 && fmt != 0xFFFE)  /* PCM (or extensible) only */
                return -1;
            if ((w->channels != 1 && w->channels != 2) ||
                (w->bits != 8 && w->bits != 16) || w->rate == 0)
                return -1;
            have_fmt = 1;
        } else if (id == 0x61746164u /* "data" */) {
            w->data_off = off + 8;
            w->data_len = clen;
        }
        off += 8 + clen + (clen & 1);       /* chunks are 16-bit aligned */
    }

    if (!have_fmt || !w->data_len)
        return -1;

    uint32_t frame_bytes = (uint32_t)w->channels * (w->bits / 8);
    w->src_frames = w->data_len / frame_bytes;
    w->out_total  = (uint32_t)((uint64_t)w->src_frames * RATE / w->rate);
    w->cur = 0;
    w->out_pos = 0;
    return 0;
}

/* Fetch source frame `idx` as two S16 channels. */
static void fetch(const struct wav *w, uint32_t idx, int *l, int *r)
{
    uint32_t fb = (uint32_t)w->channels * (w->bits / 8);
    const uint8_t *p = w->data + w->data_off + idx * fb;
    if (w->bits == 16) {
        *l = (int16_t)rd16(p);
        *r = w->channels == 2 ? (int16_t)rd16(p + 2) : *l;
    } else {                                /* 8-bit is unsigned, centered at 128 */
        *l = ((int)p[0] - 128) << 8;
        *r = w->channels == 2 ? (((int)p[1] - 128) << 8) : *l;
    }
}

int wav_fill(struct wav *w, int16_t *out, int nframes)
{
    uint64_t step = ((uint64_t)w->rate << 16) / RATE;   /* Q16 src frames/out */
    int n = 0;
    for (; n < nframes; n++) {
        uint32_t idx = (uint32_t)(w->cur >> 16);
        if (idx >= w->src_frames)
            break;                          /* end of file */
        int l, r;
        fetch(w, idx, &l, &r);
        out[2 * n]     = (int16_t)l;
        out[2 * n + 1] = (int16_t)r;
        w->cur += step;
        w->out_pos++;
    }
    return n;
}

void wav_seek(struct wav *w, uint32_t out_frame)
{
    if (out_frame > w->out_total) out_frame = w->out_total;
    w->cur = ((uint64_t)out_frame * w->rate << 16) / RATE;
    w->out_pos = out_frame;
}

uint32_t wav_pos(const struct wav *w)   { return w->out_pos; }
uint32_t wav_total(const struct wav *w) { return w->out_total; }
