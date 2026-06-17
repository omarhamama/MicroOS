#pragma once

#include <stdint.h>

int  vsnd_init(void);               /* 0 = sound card found and live */
int  vsnd_present(void);

/* Play a square-wave tone, blocking until it finishes.
 * Returns 0, or -1 with the failing step printed. (No-op if the stream
 * is currently owned by the music player — see vsnd_open.) */
int vsnd_beep(uint32_t freq_hz, uint32_t ms);

/* ---- streaming playback (the MP3/music player) ----------------------
 *
 * beep() owns the stream for its whole duration; a music player instead
 * holds it open across many buffers. The contract:
 *
 *   vsnd_open()                 claim + start the output stream (44.1k
 *                               S16 stereo). Returns 0, or -1 if the
 *                               card is missing or already in use.
 *   vsnd_submit(frames, n)      hand the card `n` stereo frames (4 bytes
 *                               each) and BLOCK until it has played them
 *                               — completion is playback, so this is the
 *                               player's clock. Returns 0 / -1.
 *   vsnd_close()                stop + release; the stream is free again.
 *
 * Only one owner at a time (the audio device has a single output stream).
 */
int  vsnd_open(void);
int  vsnd_submit(const int16_t *frames, uint32_t nframes);
void vsnd_close(void);
int  vsnd_inuse(void);
