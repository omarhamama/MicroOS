#pragma once

/*
 * player.h — the music player engine (the brain behind the Winamp window).
 *
 * It owns a background task that pulls PCM from the current source (the
 * built-in chiptune, a .wav, or — once built — a .mp3), pushes it to the
 * sound card, and on the way computes a spectrum for the visualizer. The
 * GUI just calls these to drive the transport and read state to paint.
 */

#include <stdint.h>

#define PLAYER_NBANDS  16       /* spectrum-analyzer bars */
#define PLAYER_BAR_MAX 64       /* a bar's value at full deflection */

enum player_state { PS_STOPPED, PS_PLAYING, PS_PAUSED };

void player_init(void);         /* scan for tracks + start the audio task */

/* Transport. */
void player_toggle(void);       /* the big play/pause button */
void player_play(void);
void player_pause(void);
void player_stop(void);
void player_next(void);
void player_prev(void);
void player_seek_frac(int permille);     /* jump to 0..1000 of the track */
void player_set_volume(int v);           /* 0..256 */
int  player_volume(void);

/* Playlist. */
int          player_track_count(void);
const char  *player_track_name(int i);
int          player_current_track(void);
void         player_select(int i);       /* load track i and play it */

/* State for the UI. */
int          player_get_state(void);     /* enum player_state */
uint32_t     player_pos_frames(void);
uint32_t     player_total_frames(void);
const char  *player_title(void);         /* current track's display name */
const char  *player_status(void);        /* a one-line note, or "" */
/* Copy up to `max` spectrum bar values (0..PLAYER_BAR_MAX); returns count. */
int          player_spectrum(int *bars, int max);
