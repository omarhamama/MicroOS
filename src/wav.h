#pragma once

/*
 * wav.h — the simplest real audio format: PCM in a RIFF/WAVE wrapper.
 *
 * A .wav file is a header followed by the raw samples, uncompressed — no
 * decoder needed, just parse the header and read numbers. We accept the
 * common cases (8- or 16-bit, mono or stereo, any sample rate) and
 * resample on the fly to the card's native 44100 Hz stereo. This is the
 * player's "real file" path; MP3 (which IS compressed) is the big codec.
 */

#include <stdint.h>

struct wav {
    const uint8_t *data;
    uint32_t len;
    uint32_t data_off, data_len;    /* the PCM bytes within the file */
    uint32_t rate;                  /* source sample rate */
    int      channels;              /* 1 or 2 */
    int      bits;                  /* 8 or 16 */
    uint32_t src_frames;            /* total frames at the source rate */
    uint32_t out_total;             /* total frames once resampled to 44.1k */
    uint64_t cur;                   /* Q16 cursor into source frames */
    uint32_t out_pos;               /* output frame position (for the UI) */
};

/* Validate + parse `file`; 0 on success, -1 if it isn't a WAV we grok. */
int      wav_open(struct wav *w, const uint8_t *file, uint32_t len);
/* Produce up to nframes of 44.1k S16 stereo; returns frames written
 * (< nframes, possibly 0, at end of file). */
int      wav_fill(struct wav *w, int16_t *stereo, int nframes);
void     wav_seek(struct wav *w, uint32_t out_frame);
uint32_t wav_pos(const struct wav *w);
uint32_t wav_total(const struct wav *w);
