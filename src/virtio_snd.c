/*
 * virtio_snd.c — the sound card.
 *
 * Sound, demystified: audio is a list of numbers — air-pressure
 * samples, 44100 of them per second — and a sound card is a device
 * that consumes that list AT EXACTLY THAT RATE and wiggles a speaker
 * to match. Everything else is bookkeeping around one hard fact: the
 * hardware must never run out of numbers mid-song (the dreaded
 * "buffer underrun"), so audio is always submitted in buffers ahead
 * of playback.
 *
 * virtio-snd splits the work across two queues:
 *
 *   CONTROL queue — small request/response transactions, like a tiny
 *     RPC system: "set stream 0 to 44.1 kHz stereo S16" (SET_PARAMS),
 *     then PREPARE, then START. Each request is a [request][response]
 *     descriptor pair; the device fills the response with a status.
 *
 *   TX queue — the actual samples, each buffer wrapped in a tiny
 *     header (which stream?) and tailed by a device-written status.
 *     The device completes each buffer AS IT PLAYS it — which is why
 *     beep() takes half a second to return for a half-second beep:
 *     completion IS playback progress.
 *
 * The tone itself is a square wave: the simplest waveform there is,
 * +amplitude for half a cycle, -amplitude for the other half. It's
 * the sound of every 1980s computer for a reason — you can make it
 * with a counter and a sign flip, no floating point required (good,
 * because kernel code compiles with the FPU off).
 */

#include "virtio_snd.h"
#include "virtio.h"
#include "task.h"
#include "lib.h"
#include "kprintf.h"

#define QSIZE 4

/* control request codes (virtio spec 5.14) */
#define R_PCM_SET_PARAMS 0x0101
#define R_PCM_PREPARE    0x0102
#define R_PCM_RELEASE    0x0103
#define R_PCM_START      0x0104
#define R_PCM_STOP       0x0105
#define S_OK             0x8000

#define FMT_S16   5                 /* signed 16-bit samples */
#define RATE_44K  6                 /* 44100 Hz */
#define RATE_HZ   44100u
#define CHANNELS  2u
#define PERIOD_BYTES 4096u          /* one tx buffer of audio */

static struct virtio_dev dev;
static struct virtq ctlq, txq;

static struct virtq_desc ctl_desc[QSIZE]  __attribute__((aligned(16)));
static uint8_t ctl_avail[6 + 2 * QSIZE]   __attribute__((aligned(2)));
static uint8_t ctl_used[6 + 8 * QSIZE]    __attribute__((aligned(4)));
static struct virtq_desc tx_desc[QSIZE]   __attribute__((aligned(16)));
static uint8_t tx_avail[6 + 2 * QSIZE]    __attribute__((aligned(2)));
static uint8_t tx_used[6 + 8 * QSIZE]     __attribute__((aligned(4)));

int vsnd_present(void) { return dev.base != 0; }

/* Exactly one thing may drive the single output stream at a time: a
 * blocking beep, OR the music player holding the stream open across many
 * buffers. This flag keeps them from stepping on each other's queues. */
static volatile int inuse;
int vsnd_inuse(void) { return inuse; }

static void vsnd_isr(void)
{
    virtio_irq_ack(&dev);
    task_wakeup(&ctlq);             /* whoever awaits a control reply */
    task_wakeup(&txq);              /* or a played-out PCM buffer */
}

int vsnd_init(void)
{
    if (virtio_find(25 /* sound */, 0, &dev) < 0)
        return -1;
    if (virtio_queue_init(&dev, &ctlq, 0, QSIZE, ctl_desc, ctl_avail, ctl_used) < 0 ||
        virtio_queue_init(&dev, &txq, 2, QSIZE, tx_desc, tx_avail, tx_used) < 0)
        return -1;
    virtio_set_irq_handler(&dev, vsnd_isr);
    virtio_driver_ok(&dev);
    return 0;
}

/* One control transaction: send `req`, wait for the 4-byte status. */
static int control(const void *req, uint32_t len)
{
    static volatile uint32_t status;
    status = 0;

    ctl_desc[0] = (struct virtq_desc){ (uint64_t)req, len,
                                       VIRTQ_DESC_F_NEXT, 1 };
    ctl_desc[1] = (struct virtq_desc){ (uint64_t)&status, 4,
                                       VIRTQ_DESC_F_WRITE, 0 };
    virtq_kick(&ctlq, 0);

    uint32_t id, ulen;
    asm volatile("msr daifset, #2");
    while (virtq_pop_used(&ctlq, &id, &ulen) < 0)
        task_block(&ctlq);
    asm volatile("msr daifclr, #2");
    return status == S_OK ? 0 : -(int)status;
}

/* The simple commands share one shape: { code, stream_id }. */
static int pcm_cmd(uint32_t code)
{
    static struct {
        uint32_t code, stream_id;
    } req;
    req.code = code;
    req.stream_id = 0;              /* stream 0: the output stream */
    return control(&req, sizeof(req));
}

static int pcm_set_params(void)
{
    static struct {
        uint32_t code, stream_id;
        uint32_t buffer_bytes, period_bytes, features;
        uint8_t  channels, format, rate, pad;
    } __attribute__((packed)) req;

    memset(&req, 0, sizeof(req));
    req.code = R_PCM_SET_PARAMS;
    req.stream_id = 0;
    req.buffer_bytes = PERIOD_BYTES * 4;
    req.period_bytes = PERIOD_BYTES;
    req.channels = CHANNELS;
    req.format = FMT_S16;
    req.rate = RATE_44K;
    return control(&req, sizeof(req));
}

/* Send one buffer of samples and wait until the card has PLAYED it. */
static int pcm_write(const void *pcm, uint32_t len)
{
    static const uint32_t stream0 = 0;          /* the xfer header */
    static volatile struct {
        uint32_t status, latency_bytes;
    } resp;

    tx_desc[0] = (struct virtq_desc){ (uint64_t)&stream0, 4,
                                      VIRTQ_DESC_F_NEXT, 1 };
    tx_desc[1] = (struct virtq_desc){ (uint64_t)pcm, len,
                                      VIRTQ_DESC_F_NEXT, 2 };
    tx_desc[2] = (struct virtq_desc){ (uint64_t)&resp, 8,
                                      VIRTQ_DESC_F_WRITE, 0 };
    virtq_kick(&txq, 0);

    /* This block is where a half-second beep "takes" half a second:
     * the task sleeps while the card plays, other tasks run freely. */
    uint32_t id, ulen;
    asm volatile("msr daifset, #2");
    while (virtq_pop_used(&txq, &id, &ulen) < 0)
        task_block(&txq);
    asm volatile("msr daifclr, #2");
    return resp.status == S_OK ? 0 : -1;
}

/* set_params -> prepare -> start, the three-step ritual that brings the
 * output stream live. Shared by beep() and the streaming player. */
static int stream_start(void)
{
    int rc;
    if ((rc = pcm_set_params()) < 0 ||
        (rc = pcm_cmd(R_PCM_PREPARE)) < 0 ||
        (rc = pcm_cmd(R_PCM_START)) < 0)
        return rc;
    return 0;
}

static void stream_stop(void)
{
    pcm_cmd(R_PCM_STOP);
    pcm_cmd(R_PCM_RELEASE);
}

/* ---- streaming API: the music player holds the stream open ---------- */

int vsnd_open(void)
{
    if (!dev.base || inuse)
        return -1;
    if (stream_start() < 0)
        return -1;
    inuse = 1;
    return 0;
}

/* Feed the card nframes of S16 stereo, one period-buffer at a time,
 * blocking until each has played. That blocking IS the player's clock:
 * the task sleeps in the kernel while the speaker catches up. */
int vsnd_submit(const int16_t *frames, uint32_t nframes)
{
    if (!inuse)
        return -1;
    const uint32_t per = PERIOD_BYTES / (2 * CHANNELS);     /* frames/buffer */
    while (nframes) {
        uint32_t n = nframes < per ? nframes : per;
        if (pcm_write(frames, n * 2 * CHANNELS) < 0)
            return -1;
        frames  += n * CHANNELS;
        nframes -= n;
    }
    return 0;
}

void vsnd_close(void)
{
    if (!inuse)
        return;
    stream_stop();
    inuse = 0;
}

int vsnd_beep(uint32_t freq_hz, uint32_t ms)
{
    if (!dev.base || inuse)         /* player owns the stream? skip the beep */
        return -1;
    if (freq_hz < 20 || freq_hz > 20000 || ms < 10 || ms > 10000)
        return -1;

    int rc;
    if ((rc = stream_start()) < 0) {
        kprintf("beep: stream setup failed (status 0x%x)\n", -rc);
        return -1;
    }

    /* Generate and stream the square wave, one period-buffer at a
     * time. 16-bit stereo at 44.1 kHz = 4 bytes per sample frame. */
    static int16_t buf[PERIOD_BYTES / 2];
    uint32_t total_frames = RATE_HZ * ms / 1000;
    uint32_t half_wave = RATE_HZ / (2 * freq_hz);   /* frames per flip */
    uint32_t phase = 0;
    int16_t level = 6000;                           /* comfortable volume */

    while (total_frames) {
        uint32_t frames = PERIOD_BYTES / (2 * CHANNELS);
        if (frames > total_frames)
            frames = total_frames;

        for (uint32_t i = 0; i < frames; i++) {
            if (++phase >= half_wave) {
                phase = 0;
                level = (int16_t)-level;            /* the sign flip IS the sound */
            }
            buf[i * 2]     = level;                 /* left */
            buf[i * 2 + 1] = level;                 /* right */
        }
        if (pcm_write(buf, frames * 2 * CHANNELS) < 0)
            break;
        total_frames -= frames;
    }

    stream_stop();
    return 0;
}
