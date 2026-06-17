/*
 * player.c — the playback engine.
 *
 * One background task is the whole story. It loops: pull a buffer of PCM
 * from the current source, scale it by the volume, analyse it for the
 * visualizer, and hand it to the sound card. vsnd_submit() blocks until
 * the card has played the buffer, so the loop runs in lock-step with the
 * speaker — no timers, no clock math: playback progress IS the clock.
 *
 * THE SPECTRUM ANALYSER, honestly. A real one runs an FFT; we have no
 * FPU and no time for one per buffer. Instead we use a bank of one-pole
 * low-pass filters at geometrically-spaced cutoffs (a "filterbank"). The
 * energy BETWEEN two adjacent cutoffs is the energy in that frequency
 * band — bass at the bottom, treble at the top. It's a coarse, real
 * frequency analysis built from nothing but shifts and adds, and it makes
 * the bars dance convincingly to the beat. That's the goal.
 */

#include "player.h"
#include "synth.h"
#include "wav.h"
#include "mp3.h"
#include "virtio_snd.h"
#include "task.h"
#include "timer.h"
#include "fs.h"
#include "lib.h"
#include "kprintf.h"

#define FRAMES 1024                 /* PCM frames per buffer (~23 ms)      */
#define RATE   44100u

enum src_kind { SRC_SYNTH, SRC_WAV, SRC_MP3 };

struct track {
    char name[40];
    struct fs_node *node;           /* file backing it (NULL = synth)      */
    enum src_kind kind;
};

#define MAXTRACKS 24
static struct track tracks[MAXTRACKS];
static int ntracks;
static int cur_track;

static volatile int state = PS_STOPPED;
static enum src_kind src;
static struct wav wav;
static struct mp3dec *mp3;
static int volume = 200;            /* 0..256 */
static char status[48];

static int16_t pcmbuf[FRAMES * 2];  /* the working buffer (static: off-stack) */

/* ---- spectrum analyser (integer one-pole filterbank) ---------------- */

#define NLP (PLAYER_NBANDS - 1)     /* one fewer low-pass than bands */
/* Cutoff "alpha" coefficients in Q16, geometric from deep bass (~40 Hz)
 * up to ~4 kHz. Higher alpha = higher cutoff. Hand-spaced by ear/eye. */
static const int32_t alpha[NLP] = {
    400, 555, 770, 1068, 1481, 2055, 2850, 3953,
    5483, 7604, 10546, 14627, 20287, 28138, 39024,
};
/* Per-band display gain (treble is quieter, so boost it). Tuned by eye. */
static const int32_t gain[PLAYER_NBANDS] = {
    20, 22, 24, 27, 30, 34, 38, 43, 48, 54, 60, 66, 72, 78, 84, 90,
};

static int32_t lp[NLP];                     /* filter memory across buffers */
static volatile int bars[PLAYER_NBANDS];    /* what the GUI draws */

static uint32_t isqrt32(uint64_t n)
{
    uint64_t x = 0, b = 1ULL << 40;
    while (b > n) b >>= 2;
    while (b) {
        if (n >= x + b) { n -= x + b; x = (x >> 1) + b; }
        else x >>= 1;
        b >>= 2;
    }
    return (uint32_t)x;
}

static void analyse(const int16_t *buf, int n)
{
    int64_t energy[PLAYER_NBANDS];
    memset(energy, 0, sizeof energy);

    for (int i = 0; i < n; i++) {
        int x = (buf[2 * i] + buf[2 * i + 1]) >> 1;     /* mono mix */
        int prev = 0;
        for (int k = 0; k < NLP; k++) {
            lp[k] += (int32_t)(((int64_t)(x - lp[k]) * alpha[k]) >> 16);
            int band = lp[k] - prev;
            prev = lp[k];
            energy[k] += (int64_t)band * band;
        }
        int top = x - prev;                             /* above the top cutoff */
        energy[PLAYER_NBANDS - 1] += (int64_t)top * top;
    }

    for (int k = 0; k < PLAYER_NBANDS; k++) {
        uint32_t rms = isqrt32((uint64_t)(energy[k] / (n ? n : 1)));
        int target = (int)(((int64_t)rms * gain[k]) >> 12);
        if (target > PLAYER_BAR_MAX) target = PLAYER_BAR_MAX;
        if (target >= bars[k]) bars[k] = target;        /* snappy attack */
        else { bars[k] -= 3; if (bars[k] < 0) bars[k] = 0; }   /* gravity */
    }
}

static void spectrum_decay(void)        /* when paused/stopped, settle to 0 */
{
    for (int k = 0; k < PLAYER_NBANDS; k++) {
        bars[k] -= 4;
        if (bars[k] < 0) bars[k] = 0;
    }
}

int player_spectrum(int *out, int max)
{
    int n = max < PLAYER_NBANDS ? max : PLAYER_NBANDS;
    for (int i = 0; i < n; i++) out[i] = bars[i];
    return n;
}

/* ---- the playlist --------------------------------------------------- */

static int ends_with(const char *s, const char *suf)
{
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    if (ls < lf) return 0;
    for (int i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;      /* case-insensitive */
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void add_track(struct fs_node *n, enum src_kind kind)
{
    if (ntracks >= MAXTRACKS) return;
    struct track *t = &tracks[ntracks++];
    t->node = n;
    t->kind = kind;
    int i = 0;
    for (; n->name[i] && i < (int)sizeof(t->name) - 1; i++) t->name[i] = n->name[i];
    t->name[i] = '\0';
}

static void scan_dir(struct fs_node *dir)       /* recurse, collecting audio */
{
    for (struct fs_node *c = dir->children; c; c = c->next) {
        if (c->type == FS_DIR) scan_dir(c);
        else if (ends_with(c->name, ".wav")) add_track(c, SRC_WAV);
        else if (ends_with(c->name, ".mp3")) add_track(c, SRC_MP3);
    }
}

int         player_track_count(void)      { return ntracks; }
const char *player_track_name(int i)
{
    return (i >= 0 && i < ntracks) ? tracks[i].name : "";
}
int         player_current_track(void)    { return cur_track; }
int         player_get_state(void)        { return state; }
int         player_volume(void)           { return volume; }
void        player_set_volume(int v)      { volume = v < 0 ? 0 : v > 256 ? 256 : v; }
const char *player_status(void)           { return status; }
const char *player_title(void)
{
    return (cur_track >= 0 && cur_track < ntracks) ? tracks[cur_track].name : "";
}

/* ---- sources -------------------------------------------------------- */

static int load_track(int i)        /* prepare source i; 0 = playable */
{
    if (i < 0 || i >= ntracks) return -1;
    if (mp3) { mp3_close(mp3); mp3 = 0; }       /* free any previous decoder */
    cur_track = i;
    src = tracks[i].kind;
    status[0] = '\0';

    if (src == SRC_SYNTH) {
        synth_reset();
        return 0;
    }
    if (src == SRC_WAV) {
        struct fs_node *n = tracks[i].node;
        if (!n->data || wav_open(&wav, (const uint8_t *)n->data, (uint32_t)n->size) < 0) {
            ksprintf(status, sizeof status, "Not a PCM WAV file");
            return -1;
        }
        return 0;
    }
    /* SRC_MP3 */
    {
        struct fs_node *n = tracks[i].node;
        if (!n->data ||
            !(mp3 = mp3_open((const uint8_t *)n->data, (uint32_t)n->size))) {
            ksprintf(status, sizeof status, "Not an MPEG-1 Layer III file");
            return -1;
        }
        ksprintf(status, sizeof status, "MPEG-1 Layer III  %d kbps  %d Hz %s",
                 mp3_bitrate(mp3), mp3_rate(mp3),
                 mp3_channels(mp3) == 1 ? "mono" : "stereo");
        return 0;
    }
}

/* Fill `buf` with up to FRAMES frames; returns frames actually produced
 * (fewer than FRAMES means the source ran out). */
static int source_fill(int16_t *buf, int n)
{
    switch (src) {
    case SRC_SYNTH: synth_fill(buf, n); return n;       /* loops forever */
    case SRC_WAV:   return wav_fill(&wav, buf, n);
    case SRC_MP3:   return mp3 ? mp3_fill(mp3, buf, n) : 0;
    default:        return 0;
    }
}

uint32_t player_pos_frames(void)
{
    if (src == SRC_SYNTH) return synth_pos();
    if (src == SRC_WAV)   return wav_pos(&wav);
    if (src == SRC_MP3)   return mp3 ? mp3_pos(mp3) : 0;
    return 0;
}
uint32_t player_total_frames(void)
{
    if (src == SRC_SYNTH) return synth_total_frames();
    if (src == SRC_WAV)   return wav_total(&wav);
    if (src == SRC_MP3)   return mp3 ? mp3_total(mp3) : 0;
    return 0;
}

void player_seek_frac(int permille)
{
    uint32_t total = player_total_frames();
    if (!total) return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    uint32_t f = (uint32_t)((uint64_t)total * permille / 1000);
    if (src == SRC_SYNTH) synth_seek(f);
    else if (src == SRC_WAV) wav_seek(&wav, f);
    else if (src == SRC_MP3 && mp3) mp3_seek(mp3, f);
}

/* ---- transport ------------------------------------------------------ */

void player_play(void)
{
    if (state == PS_PAUSED) { state = PS_PLAYING; return; }
    if (load_track(cur_track) == 0) state = PS_PLAYING;
    else state = PS_STOPPED;
}
void player_pause(void)  { if (state == PS_PLAYING) state = PS_PAUSED; }
void player_stop(void)   { state = PS_STOPPED; }
void player_toggle(void)
{
    if (state == PS_PLAYING) state = PS_PAUSED;
    else                     player_play();
}
void player_select(int i)
{
    if (i < 0 || i >= ntracks) return;
    if (load_track(i) == 0) state = PS_PLAYING;
    else                    state = PS_STOPPED;
}
void player_next(void) { if (ntracks) player_select((cur_track + 1) % ntracks); }
void player_prev(void) { if (ntracks) player_select((cur_track + ntracks - 1) % ntracks); }

/* ---- the audio task ------------------------------------------------- */

static void player_task(uint64_t arg)
{
    (void)arg;
    int opened = 0;

    for (;;) {
        int st = state;

        if (st == PS_PLAYING || st == PS_PAUSED) {
            if (!opened) {
                if (vsnd_open() < 0) {          /* device busy (e.g. a beep) */
                    state = PS_STOPPED;
                    ksprintf(status, sizeof status, "Audio device busy");
                    continue;
                }
                opened = 1;
            }

            if (st == PS_PLAYING) {
                int got = source_fill(pcmbuf, FRAMES);
                if (got < FRAMES) {
                    memset(pcmbuf + got * 2, 0, (FRAMES - got) * 2 * sizeof(int16_t));
                    if (got == 0) {             /* end of track: advance */
                        player_next();
                        continue;
                    }
                }
                /* volume + analysis on the real samples */
                if (volume != 256)
                    for (int i = 0; i < FRAMES * 2; i++)
                        pcmbuf[i] = (int16_t)((pcmbuf[i] * volume) >> 8);
                analyse(pcmbuf, FRAMES);
            } else {                            /* paused: feed silence */
                memset(pcmbuf, 0, sizeof pcmbuf);
                spectrum_decay();
            }

            vsnd_submit(pcmbuf, FRAMES);        /* blocks ~23 ms (the clock) */
        } else {                                /* stopped */
            if (opened) { vsnd_close(); opened = 0; }
            spectrum_decay();
            task_sleep(TIMER_HZ / 30);
        }
    }
}

void player_init(void)
{
    /* Track 0 is always the built-in chiptune; then any audio on disk. */
    struct track *t = &tracks[ntracks++];
    t->node = 0;
    t->kind = SRC_SYNTH;
    {
        const char *s = synth_title();
        int i = 0;
        for (; s[i] && i < (int)sizeof(t->name) - 1; i++) t->name[i] = s[i];
        t->name[i] = '\0';
    }
    if (fs_root) scan_dir(fs_root);

    cur_track = 0;
    src = SRC_SYNTH;
    synth_reset();
    status[0] = '\0';

    /* The MP3 decoder is float-heavy and recursion-ish; give it room. */
    task_create_stack("player", player_task, 0, 64 * 1024);
    kprintf("[boot] music player: %d track(s), try the Music dock icon\n", ntracks);
}
