#pragma once

/* A tiny HTTP/HTML browser, rendered into a GUI window. */

void browser_init(void);                 /* start the fetch task + home page */
void browser_navigate(const char *url);  /* request a page (shell or link click) */

/* GUI hooks (called from the Browser window's body in gui.c): */
void browser_draw(int x, int y, int w, int h);  /* render page, record link rects */
void browser_click(int mx, int my);             /* follow a link if one was hit */
void browser_back(void);                         /* history: previous page */
const char *browser_status(void);                /* current URL / "Loading…" */
void browser_scroll(int dpx);                    /* scroll the page (+down/-up) */

/* Editable address bar: click to focus, type a URL, Enter to navigate.
 * gui.c forwards on-screen keystrokes here when the bar is focused. */
void browser_addr_focus(void);
void browser_addr_blur(void);
int  browser_addr_editing(void);
int  browser_addr_key(char c);          /* returns 1 if the key was consumed */
const char *browser_addr_text(void);    /* text to show in the field */
int  browser_addr_cursor(void);         /* caret index within the URL text */
void browser_addr_arrow(int dir);       /* move caret: 0=L 1=R 2=home 3=end */
void browser_addr_clickfield(int px);   /* click on the field: focus or move caret */

/* Security state of the current page, for the lock indicator:
 *   0 = plain HTTP (no security)
 *   1 = HTTPS, certificate validated  (security_text() = "root name")
 *  -1 = HTTPS, validation FAILED       (security_text() = the reason) */
int  browser_security(void);
const char *browser_security_text(void);
