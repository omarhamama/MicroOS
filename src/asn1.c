/*
 * asn1.c — DER TLV decoding.
 *
 * The only subtlety is the length field. A length byte < 0x80 IS the
 * length (short form). Otherwise its low 7 bits say how many following
 * bytes hold the length, big-endian (long form): 0x82 0x05 0xDC means
 * "the next 0x05DC = 1500 bytes". We reject the indefinite form (0x80),
 * which DER forbids anyway.
 */

#include "asn1.h"

int der_next(struct der *d, int *tag, const uint8_t **val, size_t *len)
{
    if (d->p >= d->end)
        return -1;
    int t = *d->p++;
    if (d->p >= d->end)
        return -1;

    size_t l;
    uint8_t b = *d->p++;
    if (b < 0x80) {
        l = b;                              /* short form */
    } else {
        int nb = b & 0x7f;
        if (nb == 0 || nb > 4 || d->p + nb > d->end)
            return -1;                      /* indefinite or absurd length */
        l = 0;
        for (int i = 0; i < nb; i++)
            l = (l << 8) | *d->p++;
    }
    if (d->p + l > d->end)
        return -1;                          /* value runs past the buffer */

    if (tag) *tag = t;
    if (val) *val = d->p;
    if (len) *len = l;
    d->p += l;
    return 0;
}

int der_expect(struct der *d, int want, const uint8_t **val, size_t *len)
{
    int tag;
    if (der_next(d, &tag, val, len) < 0)
        return -1;
    return tag == want ? 0 : -1;
}
