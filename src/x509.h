#pragma once

/*
 * x509.h — parse a DER certificate into the fields chain validation needs.
 *
 * A certificate is a signed statement: "this public key belongs to this
 * name", signed by an issuer. To trust a server we need to pull out,
 * without copying, pointers into the cert for:
 *   - tbs        : the exact bytes the signature covers
 *   - sig + alg  : the signature and which algorithm signed it
 *   - issuer/subject : Distinguished Names, compared to chain certs
 *   - validity   : not-before / not-after, checked against the clock
 *   - public key : RSA modulus+exponent, or an EC point
 *   - SAN        : the DNS names this cert is valid for
 */

#include <stdint.h>
#include <stddef.h>

enum {
    SIG_UNKNOWN = 0,
    SIG_RSA_SHA256, SIG_RSA_SHA384, SIG_RSA_SHA512,
    SIG_ECDSA_SHA256, SIG_ECDSA_SHA384,
};
enum { KEY_UNKNOWN = 0, KEY_RSA, KEY_EC_P256 };

struct x509 {
    const uint8_t *raw;     size_t raw_len;
    const uint8_t *tbs;     size_t tbs_len;     /* signed region (incl. tag) */
    int            sig_alg;
    const uint8_t *sig;     size_t sig_len;     /* signatureValue contents   */

    const uint8_t *issuer;  size_t issuer_len;  /* Name DER, incl. tag       */
    const uint8_t *subject; size_t subject_len;

    int64_t not_before, not_after;              /* YYYYMMDDhhmmss as integer */

    int            key_alg;
    const uint8_t *rsa_n;   size_t rsa_n_len;   /* leading 0x00 stripped     */
    const uint8_t *rsa_e;   size_t rsa_e_len;
    uint8_t        ec_point[65]; size_t ec_point_len;

    const uint8_t *san;     size_t san_len;     /* SEQUENCE OF GeneralName    */
    int            is_ca;
};

/* Parse `der` (length `len`) into *out. Returns 0 on success, -1 on
 * malformed/truncated input. Pointers in *out alias `der`, so keep it
 * alive as long as *out is used. */
int x509_parse(struct x509 *out, const uint8_t *der, size_t len);

/* Does this cert's SAN (or, as a fallback, nothing) cover `host`?
 * Returns 1 if a dNSName matches (incl. a leading "*." wildcard). */
int x509_matches_host(const struct x509 *c, const char *host);

/* Verify that `cert` was signed by the private key matching `issuer`'s
 * public key. Dispatches on the signature algorithm (RSA PKCS#1 v1.5 or
 * ECDSA, with SHA-256/384/512). Returns 0 if the signature checks out. */
int x509_check_sig(const struct x509 *cert, const struct x509 *issuer);

/* Validate a full chain (server sends leaf first, then intermediates):
 *   certs[],lens[],ncerts : the DER certificates from the handshake
 *   host                  : expected server name (SAN check on the leaf)
 *   now                   : current time as YYYYMMDDhhmmss (0 = skip dates)
 *   err,errlen            : human-readable failure reason
 * Returns 0 iff the leaf chains, via valid signatures, to one of the
 * embedded trusted roots, the dates are valid, and the host matches. */
int x509_verify_chain(const uint8_t *const *certs, const size_t *lens, int ncerts,
                      const char *host, int64_t now, char *err, int errlen,
                      char *anchor, int anchorlen);

/* Pull the commonName (CN) text out of a Name DER (issuer/subject).
 * Writes a NUL-terminated string; empty if no CN found. */
void x509_name_cn(const uint8_t *name, size_t namelen, char *out, int outlen);
