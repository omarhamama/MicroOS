/*
 * arabic.c — the Arabic joining (shaping) algorithm.
 *
 * Each letter has a joining class: dual (connects both sides), right-
 * joining (connects only to the previous letter), or non-joining. A
 * letter takes its medial form when it connects on both sides, final
 * when only to the previous, initial when only to the next, isolated
 * otherwise — and a connection requires the PRECEDING letter to be dual.
 */

#include "arabic.h"
#include "arabicfont.h"

int arabic_idx(int cp)
{
    if (cp < 0x0600 || cp > 0x06FF) return -1;   /* fast reject */
    for (int i = 0; i < arabic_nletters; i++)
        if (arabic_letters[i].base == cp) return i;
    return -1;
}

int shape_arabic(const int *cps, int n, uint8_t *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        int li = arabic_idx(cps[i]);
        if (li < 0) continue;
        const struct arabic_letter *L = &arabic_letters[li];
        int pc = 0, nc = 0;
        if (i > 0)   { int p = arabic_idx(cps[i-1]); if (p>=0) pc = arabic_letters[p].cls; }
        if (i+1 < n) { int q = arabic_idx(cps[i+1]); if (q>=0) nc = arabic_letters[q].cls; }
        int jp = (L->cls >= 1) && (pc == 2);     /* joins to previous letter */
        int jn = (L->cls == 2) && (nc >= 1);     /* joins to next letter */
        out[m++] = jp && jn ? L->med : jp ? L->fin : jn ? L->init : L->iso;
    }
    return m;
}
