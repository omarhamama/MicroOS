#pragma once

/*
 * cacerts.h — the embedded trusted-root store.
 *
 * Trust has to bottom out somewhere. A browser ships with a few hundred
 * root certificates it trusts a priori; MicroOS ships with a handful.
 * Certificate validation walks a server's chain up to one of these and
 * believes the chain only if it gets there with every signature intact.
 */

#include <stddef.h>

struct ca_root {
    const char          *name;      /* human label, for the UI */
    const unsigned char *der;       /* the root certificate, DER */
    size_t               len;
};

extern const struct ca_root ca_roots[];
extern const int ca_roots_count;
