#pragma once

#include <stdint.h>

void gui_init(void);            /* start the desktop (no-op without ramfb) */
int  gui_active(void);
uint64_t gui_frames(void);

/* Called by uart_putc for every character that crosses the serial
 * port — the GUI's console window mirrors the whole conversation. */
void gui_console_feed(char c);

/* Offer a typed key to the GUI (e.g. the browser's focused address bar).
 * Returns 1 if the GUI consumed it; 0 means route it to the shell. */
int gui_handle_key(char c);

/* Scroll the focused page by `amount` px (+down/-up); 1 if consumed. */
int gui_scroll(int amount);

/* Move the focused TextEdit caret (0=left 1=right 2=up 3=down); 1 if
 * consumed (the editor is the front window). */
int gui_arrow(int dir);
