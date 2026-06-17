#pragma once

/*
 * mp3.h — streaming MPEG-1 Layer III decode for the music player.
 *
 * Open an in-memory .mp3, then pull 44.1 kHz / 16-bit / stereo frames as
 * the player needs them (the decoder resamples its native rate on the
 * fly). See mp3.c for the provenance of the decode core.
 */

#include <stdint.h>

struct mp3dec;

/* Parse the header (skipping an ID3v2 tag); 0/NULL if it isn't an
 * MPEG-1 Layer III stream we can decode. */
struct mp3dec *mp3_open(const uint8_t *data, uint32_t len);

/* Produce up to nframes of 44.1k S16 stereo; returns frames written
 * (< nframes, possibly 0, at end of stream). */
int      mp3_fill(struct mp3dec *m, int16_t *stereo, int nframes);
void     mp3_seek(struct mp3dec *m, uint32_t out_frame);
uint32_t mp3_pos(struct mp3dec *m);     /* current 44.1k output frame */
uint32_t mp3_total(struct mp3dec *m);   /* estimated total (from bitrate) */
int      mp3_rate(struct mp3dec *m);    /* native sample rate */
int      mp3_channels(struct mp3dec *m);
int      mp3_bitrate(struct mp3dec *m); /* kbps */
void     mp3_close(struct mp3dec *m);
