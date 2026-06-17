#pragma once

/*
 * css.h — a tiny CSS engine: enough of the cascade to be a real lesson.
 *
 * CSS is "given an element, what does it look like?" Three inputs feed
 * the answer, in increasing priority: rules from <style> matched by tag
 * ("p"), class (".note"), or id ("#main"); then the element's own
 * inline style="..."; and values inherit from the parent element. We
 * support the handful of properties our text renderer can actually obey
 * — color, font size, weight, alignment, and display:none — which is
 * exactly the point: you see how a stylesheet becomes "computed style".
 *
 * What's NOT here (and why real pages still look plain): the box model,
 * floats, fl:exbox/grid, positioning, the dozens of selectors and
 * hundreds of properties a real engine implements. This is the cascade,
 * not a layout engine.
 */

#include <stdint.h>

/* A computed style. The has_* / tri-state fields let a child start from
 * "unset" and only override what a matching rule actually specifies. */
struct css_style {
    int      has_color; uint32_t color;
    int      has_bg;    uint32_t bg;
    int      has_size;  int scale;      /* 1, 2, or 3 */
    int      bold;      /* -1 unset, 0 normal, 1 bold */
    int      center;    /* -1 unset, 0 left, 1 center */
    int      hidden;    /* -1 unset, 1 display:none */
};

void css_reset(void);                              /* clear all rules */
void css_add_stylesheet(const char *text, int len);  /* parse <style> contents */

/* Merge every rule matching (tag, cls, id) into *out, lowest priority
 * first (tag, then class, then id) — caller pre-seeds *out with the
 * inherited style. */
void css_match(const char *tag, const char *cls, const char *id,
               struct css_style *out);

/* Parse a style="..." declaration list into *out (highest priority). */
void css_parse_inline(const char *text, int len, struct css_style *out);
