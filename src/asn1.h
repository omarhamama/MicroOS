#pragma once

/*
 * asn1.h — a tiny DER reader.
 *
 * Certificates are encoded in DER, a binary form of ASN.1. Everything
 * is a nested "TLV": a Tag byte (what kind of thing — SEQUENCE,
 * INTEGER, OID, ...), a Length, then that many Value bytes. Containers
 * (SEQUENCE, SET) have Values that are themselves more TLVs. So parsing
 * a certificate is just walking this tree.
 *
 * A `der` cursor points at a span of bytes; der_next() peels off the
 * leading TLV, returns its tag and value span, and advances the cursor
 * past it. der_enter() steps INTO a container to iterate its children.
 */

#include <stdint.h>
#include <stddef.h>

/* DER tag bytes we care about. */
#define DER_BOOLEAN     0x01
#define DER_INTEGER     0x02
#define DER_BITSTRING   0x03
#define DER_OCTETSTRING 0x04
#define DER_NULL        0x05
#define DER_OID         0x06
#define DER_UTF8STRING  0x0c
#define DER_SEQUENCE    0x30
#define DER_SET         0x31
#define DER_UTCTIME     0x17
#define DER_GENTIME     0x18
/* context-specific/constructed tags appear as 0xA0, 0x80|n, etc. */

struct der { const uint8_t *p, *end; };

/* Peel one TLV off the front of `d`. On success returns 0 and fills:
 *   *tag      = the tag byte
 *   *val,*len = the value span (content only, not tag/length)
 * and advances d->p past the whole TLV. Returns -1 on malformed input. */
int der_next(struct der *d, int *tag, const uint8_t **val, size_t *len);

/* Like der_next but requires the tag to equal `want`. */
int der_expect(struct der *d, int want, const uint8_t **val, size_t *len);

/* Make a cursor over a span (e.g. to iterate a SEQUENCE's children). */
static inline struct der der_span(const uint8_t *p, size_t len)
{ struct der d = { p, p + len }; return d; }
