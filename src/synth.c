/*
 * synth.c — a tiny integer chiptune tracker.
 *
 * The whole thing is built from one idea: a PHASE ACCUMULATOR. To make a
 * tone of frequency f you add a fixed step to a 32-bit counter every
 * sample; the top bit of that counter flips f times a second, and a
 * flipping bit IS a square wave (the sound of every 8-bit console). The
 * step is `f * 2^32 / 44100`, computed once when a note starts.
 *
 * Layered on top:
 *   - a LEAD voice playing a 16th-note arpeggio (a thin 25%-duty pulse),
 *   - a BASS voice (a fat 50%-duty square, two octaves down),
 *   - a KICK (a low triangle that decays fast — a "thump"),
 *   - a HAT/SNARE (white noise from a shift register, gated short).
 *
 * A four-chord loop (Am–F–C–G, the most over-used progression in pop for
 * a reason — it just works) plays under an ascending triad arpeggio.
 * Every voice has a linear decay envelope so notes pluck instead of
 * droning. No floating point anywhere: the kernel compiles with the FPU
 * off, and a chiptune doesn't need it.
 */

#include "synth.h"
#include "lib.h"

#define RATE        44100u
#define SPS         5512u           /* samples per 16th-note step (~125ms) */
#define BAR_STEPS   16u             /* sixteenths per chord (one bar)       */
#define SONG_BARS   4u
#define SONG_STEPS  (BAR_STEPS * SONG_BARS)         /* 64 steps           */
#define LOOP_FRAMES (SPS * SONG_STEPS)              /* ~8.0 s @ 44.1k     */

/* Equal-tempered frequencies for one octave, C4..B4 (MIDI 60..71), in Hz.
 * Any other octave is just a shift — doubling frequency = up one octave. */
static const uint16_t semi_hz[12] =
    { 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494 };

/* Phase step for a MIDI note: f * 2^32 / RATE, with the octave applied
 * by shifting (each octave away from C4 halves or doubles the pitch). */
static uint32_t midi_inc(int midi)
{
    int oct = midi / 12 - 5;            /* octaves relative to C4 (MIDI 60) */
    uint32_t f = semi_hz[midi % 12];
    if (oct >= 0) f <<= oct; else f >>= -oct;
    return (uint32_t)(((uint64_t)f << 32) / RATE);
}

/* The song: four chords, each a bass root + a three-note arpeggio. */
struct chord { int bass; int arp[3]; };
static const struct chord prog[SONG_BARS] = {
    { 45, { 69, 72, 76 } },     /* Am : A2 bass,  A4 C5 E5 */
    { 41, { 65, 69, 72 } },     /* F  : F2 bass,  F4 A4 C5 */
    { 48, { 72, 76, 79 } },     /* C  : C3 bass,  C5 E5 G5 */
    { 43, { 67, 71, 74 } },     /* G  : G2 bass,  G4 B4 D5 */
};

/* ---- voice state (all advanced one sample at a time) ---------------- */

static uint32_t spos;               /* frame position within the loop */
static int      last_step = -1;

static uint32_t lead_phase, lead_inc, lead_env;
static uint32_t bass_phase, bass_inc, bass_env;
static uint32_t kick_phase, kick_inc, kick_env;
static uint32_t noise_env, noise_len;
static uint32_t lfsr = 0xACE1u;     /* the noise generator's shift register */

/* Linear decay envelope: full `peak` at t=0, ramping to 0 at t=len. */
static int env_amp(uint32_t t, int peak, uint32_t len)
{
    if (t >= len) return 0;
    return peak - (int)((uint64_t)peak * t / len);
}

/* Called once when the song advances to a new 16th-note step: pick the
 * notes for this step and (re)trigger whichever voices fire here. */
static void on_step(uint32_t step)
{
    uint32_t bar = (step / BAR_STEPS) % SONG_BARS;
    uint32_t b   = step % BAR_STEPS;            /* position within the bar */
    const struct chord *c = &prog[bar];

    /* Lead: a 4-note ascending arpeggio (triad + octave), one per step. */
    static const int pat[4] = { 0, 1, 2, 2 };
    int note = c->arp[pat[step % 4]];
    if (step % 4 == 3) note += 12;              /* sparkle on the top step */
    lead_inc = midi_inc(note);
    lead_env = 0;

    /* Bass: root on beats 1 and 3 (half notes drive the groove). */
    if (b == 0 || b == 8) {
        bass_inc = midi_inc(c->bass);
        bass_env = 0;
    }

    /* Drums: kick on 1 & 3, snare on 2 & 4, hat on every eighth. */
    if (b == 0 || b == 8) {                     /* kick */
        kick_inc = midi_inc(36);                /* ~65 Hz thump */
        kick_env = 0;
    }
    if (b == 4 || b == 12) {                    /* snare: longer noise */
        noise_env = 0;
        noise_len = 3200;
    } else if (b % 2 == 0) {                    /* hat: short noise */
        if (noise_env >= noise_len) {           /* don't cut a snare short */
            noise_env = 0;
            noise_len = 900;
        }
    }
}

void synth_reset(void)
{
    spos = 0;
    last_step = -1;
    lead_phase = bass_phase = kick_phase = 0;
    lead_env = bass_env = kick_env = 0;
    noise_env = noise_len = 0;
}

void synth_seek(uint32_t frame)
{
    spos = frame % LOOP_FRAMES;
    last_step = -1;                 /* re-trigger the step we land on */
}

uint32_t synth_pos(void)          { return spos; }
uint32_t synth_total_frames(void) { return LOOP_FRAMES; }
const char *synth_title(void)     { return "MicroOS Demo - Chiptune (Am F C G)"; }

void synth_fill(int16_t *out, int nframes)
{
    for (int i = 0; i < nframes; i++) {
        uint32_t step = (spos / SPS) % SONG_STEPS;
        if ((int)step != last_step) {
            on_step(step);
            last_step = (int)step;
        }

        int s = 0;

        /* Lead — 25% duty pulse (thin, bright). */
        int la = env_amp(lead_env, 4200, 3400);
        if (la) s += (lead_phase < 0x40000000u) ? la : -la;

        /* Bass — 50% duty square. */
        int ba = env_amp(bass_env, 6000, 6500);
        if (ba) s += (bass_phase & 0x80000000u) ? ba : -ba;

        /* Kick — triangle from the phase, fast decay. */
        int ka = env_amp(kick_env, 9000, 4200);
        if (ka) {
            int32_t p = (int32_t)(kick_phase >> 16) - 32768;   /* -32768..32767 */
            int32_t tri = p < 0 ? -p : p;                      /* fold to a /\  */
            tri = 32768 - tri;                                 /* peak in middle */
            s += (int)(((int64_t)(tri - 16384) * ka) >> 14);
        }

        /* Hat / snare — white noise via a 16-bit LFSR. */
        int na = env_amp(noise_env, 4200, noise_len ? noise_len : 1);
        if (na) {
            lfsr = (lfsr >> 1) ^ (uint32_t)(-(int)(lfsr & 1) & 0xB400);
            s += (lfsr & 1) ? na : -na;
        }

        if (s >  30000) s =  30000;
        if (s < -30000) s = -30000;
        out[2 * i]     = (int16_t)s;
        out[2 * i + 1] = (int16_t)s;

        lead_phase += lead_inc;
        bass_phase += bass_inc;
        kick_phase += kick_inc;
        lead_env++; bass_env++; kick_env++; noise_env++;

        if (++spos >= LOOP_FRAMES) { spos = 0; last_step = -1; }
    }
}
