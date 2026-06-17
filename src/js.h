#pragma once

/*
 * js.h — a tiny JavaScript interpreter for the browser's <script> tags.
 *
 * This is a real (if small) language: a lexer, a precedence-climbing
 * expression parser, and a tree-walking evaluator, with variables,
 * functions/closures, if/while/for, and a sliver of the DOM
 * (document.write, document.getElementById().textContent, console.log).
 * It's the genuine shape of how a browser runs script and mutates the
 * page.
 *
 * What it is NOT, and cannot be: a real JS engine. No prototypes, no
 * objects/arrays beyond the DOM shims, no async/Promises/fetch, no
 * regex, no closures-over-the-real-DOM. Numbers are 64-bit INTEGERS
 * (the kernel runs with the FPU off, so there are no doubles). So it
 * runs hand-written demo scripts, not React or jQuery — which is the
 * honest ceiling for a from-scratch browser.
 */

/* The browser supplies these so the interpreter can touch the page. */
struct js_host {
    void (*write)(void *ctx, const char *s);                 /* document.write */
    void (*log)(void *ctx, const char *s);                   /* console.log */
    void (*set_text)(void *ctx, const char *id, const char *s); /* el.textContent= */
    int  (*get_text)(void *ctx, const char *id, char *out, int max); /* el.textContent */
    void *ctx;
};

/* Run `src`. Returns 0 on success, -1 on a parse/runtime error (a short
 * message is written to err, if provided). */
int js_run(const char *src, struct js_host *host, char *err, int errlen);
