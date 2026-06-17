#pragma once

/*
 * synth.h — the built-in chiptune.
 *
 * Even with no audio files on disk, the player needs something to play
 * — and a from-scratch OS with no FPU is the perfect excuse for a 1980s
 * chiptune. This is a tiny tracker: a fixed song (chord progression +
 * arpeggio lead + bass + noise drums) rendered to PCM one sample at a
 * time with nothing but integer phase accumulators and square waves.
 *
 * It fills 16-bit STEREO frames (L,R interleaved) at 44100 Hz, the same
 * format the sound card wants, so the player just hands them straight on.
 */

#include <stdint.h>

void        synth_reset(void);                       /* rewind to bar 1 */
void        synth_fill(int16_t *stereo, int nframes);/* render the next N frames */
void        synth_seek(uint32_t frame);              /* jump to a frame position */
uint32_t    synth_pos(void);                         /* current frame (0..total) */
uint32_t    synth_total_frames(void);                /* length of one loop */
const char *synth_title(void);
