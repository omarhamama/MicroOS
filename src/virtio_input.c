/*
 * virtio_input.c — the mouse AND the keyboard (virtio-input devices).
 *
 * Both a tablet and a keyboard are the same kind of device — virtio
 * device ID 18 — that speak the same evdev event language Linux uses
 * (the (type, code, value) triples you'd read from /dev/input/eventN):
 *
 *   EV_ABS, ABS_X/ABS_Y, 0..32767      pointer moved (tablet)
 *   EV_KEY, BTN_LEFT (>=0x110), 1/0     mouse button
 *   EV_KEY, KEY_A (<0x100),     1/0     keyboard key
 *   EV_SYN                              end of a batch
 *
 * Because the two devices are indistinguishable except by the events
 * they send, ONE driver claims every virtio-input device, arms each
 * with its own event ring (the "conveyor belt" of empty buffers the
 * device fills), and a single handler routes by event code: absolute
 * motion and high key codes are the mouse; low key codes are the
 * keyboard, which we decode to ASCII and feed into the console input
 * queue — the very same queue the serial port feeds. So a keystroke in
 * the QEMU window reaches the shell exactly like one typed over serial.
 */

#include "virtio_input.h"
#include "virtio.h"
#include "fb.h"
#include "gui.h"
#include "uart.h"
#include "kprintf.h"

#define QSIZE  16
#define MAXDEV 2                    /* tablet + keyboard */

/* evdev constants (linux/input-event-codes.h) */
#define EV_SYN  0
#define EV_KEY  1
#define EV_REL  2
#define EV_ABS  3
#define ABS_X   0
#define ABS_Y   1
#define REL_WHEEL 8
#define BTN_LEFT  0x110
#define BTN_RIGHT 0x111
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define ABS_RANGE 32768             /* QEMU tablets report 0..32767 */

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

/* One device's worth of state. Ring memory is per-device and over-
 * aligned so each row satisfies virtio's 16/2/4-byte requirements. */
static struct virtio_dev          dev[MAXDEV];
static struct virtq               q[MAXDEV];
static struct virtq_desc          desc[MAXDEV][QSIZE]     __attribute__((aligned(16)));
static uint8_t                    avail_mem[MAXDEV][64]   __attribute__((aligned(16)));
static uint8_t                    used_mem[MAXDEV][192]   __attribute__((aligned(16)));
static struct virtio_input_event  events[MAXDEV][QSIZE];
static int                        ndev;

/* Mouse state (written in IRQ context, read by the GUI task). */
static volatile int      mx, my;
static volatile uint32_t mbuttons;
static volatile uint64_t nevents;
static volatile int      shift_down;
static volatile int      wheel_accum;       /* scroll-wheel notches, +up/-down */

int vinput_present(void)    { return ndev > 0; }
uint64_t mouse_events(void) { return nevents; }

/* Read and clear the accumulated wheel motion (notches). */
int mouse_wheel(void)
{
    int w = wheel_accum;
    wheel_accum = 0;
    return w;
}

void mouse_state(int *x, int *y, uint32_t *buttons)
{
    *x = mx;
    *y = my;
    *buttons = mbuttons;
}

/* ---- keycode -> ASCII (US layout, the main block 0..58) ----------- */

static const char keymap[] =
    /*0*/  "\0\0" "1234567890" "-=" "\b\t"
    /*16*/ "qwertyuiop" "[]" "\n" "\0"
    /*30*/ "asdfghjkl" ";'`" "\0" "\\"
    /*44*/ "zxcvbnm" ",./" "\0" "*" "\0" " ";

static const char keymap_shift[] =
    /*0*/  "\0\0" "!@#$%^&*()" "_+" "\b\t"
    /*16*/ "QWERTYUIOP" "{}" "\n" "\0"
    /*30*/ "ASDFGHJKL" ":\"~" "\0" "|"
    /*44*/ "ZXCVBNM" "<>?" "\0" "*" "\0" " ";

static void key_event(uint16_t code, uint32_t value)
{
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        shift_down = (value != 0);
        return;
    }
    if (value == 0)                 /* key release: ignore (shift handled above) */
        return;
    switch (code) {                 /* arrows: editor caret first, else scroll */
    case 105: if (gui_arrow(0))     return; break;   /* Left  */
    case 106: if (gui_arrow(1))     return; break;   /* Right */
    case 103: if (gui_arrow(2) || gui_scroll(-60))  return; break;   /* Up   */
    case 108: if (gui_arrow(3) || gui_scroll(60))   return; break;   /* Down */
    case 104: if (gui_scroll(-320)) return; break;   /* PageUp */
    case 109: if (gui_scroll(320))  return; break;   /* PageDown */
    }
    if (code >= sizeof(keymap) - 1)
        return;
    char c = shift_down ? keymap_shift[code] : keymap[code];
    if (c) {
        if (gui_handle_key(c))      /* a focused GUI field (browser URL bar)? */
            return;                 /* it ate the key */
        uart_input_push(c);         /* otherwise into the queue the shell reads */
    }
}

/* ---- the shared event handler ------------------------------------- */

static void handle_event(const struct virtio_input_event *e)
{
    nevents++;
    switch (e->type) {
    case EV_ABS:
        if (e->code == ABS_X)
            mx = (int)((uint64_t)e->value * FB_WIDTH / ABS_RANGE);
        else if (e->code == ABS_Y)
            my = (int)((uint64_t)e->value * FB_HEIGHT / ABS_RANGE);
        break;
    case EV_REL:
        if (e->code == REL_WHEEL)
            wheel_accum += (int32_t)e->value;   /* +1 = up, -1 = down */
        break;
    case EV_KEY:
        if (e->code == BTN_LEFT) {
            if (e->value) mbuttons |= MOUSE_BTN_LEFT;
            else          mbuttons &= ~MOUSE_BTN_LEFT;
        } else if (e->code == BTN_RIGHT) {
            if (e->value) mbuttons |= MOUSE_BTN_RIGHT;
            else          mbuttons &= ~MOUSE_BTN_RIGHT;
        } else if (e->code < 0x100) {
            key_event(e->code, e->value);   /* a keyboard key */
        }
        break;
    default:                        /* EV_SYN and friends: nothing to do */
        break;
    }
}

static void post_buffer(int d, uint16_t i)
{
    desc[d][i] = (struct virtq_desc){ (uint64_t)&events[d][i],
                                      sizeof(events[d][i]),
                                      VIRTQ_DESC_F_WRITE, 0 };
    virtq_kick(&q[d], i);
}

/* One ISR for all input devices: ack each and drain whatever arrived
 * (draining an idle device's empty ring is a harmless no-op). */
static void input_isr(void)
{
    for (int d = 0; d < ndev; d++) {
        virtio_irq_ack(&dev[d]);
        uint32_t id, len;
        while (virtq_pop_used(&q[d], &id, &len) == 0) {
            if (id < QSIZE && len >= sizeof(struct virtio_input_event))
                handle_event(&events[d][id]);
            post_buffer(d, (uint16_t)id);
        }
    }
}

void vinput_debug(void)
{
    kprintf("input devices: %d   mouse=%d,%d  buttons=%x  events=%lu  shift=%d\n",
            ndev, mx, my, mbuttons, nevents, shift_down);
    for (int d = 0; d < ndev; d++)
        kprintf("  dev%d base=%p irq=%u\n", d, (void *)dev[d].base, dev[d].irq);
}

int vinput_init(void)
{
    mx = FB_WIDTH / 2;              /* park the cursor mid-screen */
    my = FB_HEIGHT / 2;

    /* Claim every virtio-input device (tablet, keyboard, ...). */
    while (ndev < MAXDEV && virtio_find(18 /* input */, 0, &dev[ndev]) == 0) {
        int d = ndev;
        if (virtio_queue_init(&dev[d], &q[d], 0, QSIZE,
                              desc[d], avail_mem[d], used_mem[d]) < 0)
            break;
        virtio_set_irq_handler(&dev[d], input_isr);
        virtio_driver_ok(&dev[d]);
        for (uint16_t i = 0; i < QSIZE; i++)
            post_buffer(d, i);      /* arm the conveyor belt */
        ndev++;
    }
    return ndev ? 0 : -1;
}
