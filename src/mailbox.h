#pragma once

/*
 * mailbox.h — the Raspberry Pi's VideoCore "mailbox".
 *
 * The Pi has no ramfb. To get a framebuffer you ask the GPU (VideoCore)
 * for one through a mailbox: a shared memory message on channel 8 (the
 * "property" channel) that says "give me a 1024x768x32 screen", and the
 * GPU replies with the buffer's address and pitch. That buffer is what
 * HDMI scans out.
 */

#include <stdint.h>

/* Allocate a width×height×32 framebuffer; returns its address (NULL on
 * failure) and writes the row stride (bytes) into *pitch. */
uint32_t *mailbox_fb_init(int width, int height, int *pitch);
