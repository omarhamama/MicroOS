#pragma once

#include <stdint.h>

#define MOUSE_BTN_LEFT   1u
#define MOUSE_BTN_RIGHT  2u

int  vinput_init(void);             /* 0 = tablet found and live */
int  vinput_present(void);

/* Latest pointer state, already scaled to framebuffer pixels. */
void mouse_state(int *x, int *y, uint32_t *buttons);
int  mouse_wheel(void);             /* read+clear scroll notches (+up/-down) */
uint64_t mouse_events(void);        /* total events, for the monitor */
void vinput_debug(void);            /* dump ring state (bug hunting) */
