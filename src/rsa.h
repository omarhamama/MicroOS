#pragma once

/*
 * rsa.h — RSA signature verification (PKCS#1 v1.5).
 *
 * To verify, we raise the signature to the public exponent modulo the
 * modulus (bn_modexp). For a genuine signature that "undoes" the
 * signer's private-key operation and yields a very specific, padded
 * block: 0x00 0x01 | 0xFF...0xFF | 0x00 | DigestInfo | hash. We rebuild
 * exactly that block from the hash we expect and compare. A forger who
 * doesn't hold the private key cannot produce a signature that decrypts
 * to this rigid structure.
 */

#include <stdint.h>

/* ASN.1 DigestInfo prefixes — the algorithm-identifier bytes that sit
 * in front of the raw hash inside the padded block. */
extern const uint8_t RSA_SHA256_PREFIX[19];
extern const uint8_t RSA_SHA384_PREFIX[19];
extern const uint8_t RSA_SHA512_PREFIX[19];

/* Verify a PKCS#1 v1.5 sig over the given hash. n,e,sig are big-endian
 * byte strings. Returns 0 if valid, -1 otherwise. */
int rsa_verify(const uint8_t *n, int nlen,
               const uint8_t *e, int elen,
               const uint8_t *sig, int siglen,
               const uint8_t *hash, int hashlen,
               const uint8_t *prefix, int prefixlen);

/* Verify an RSASSA-PSS sig (MGF1, salt length == hash length — the
 * profile TLS 1.3 uses for rsa_pss_rsae_*). `hashlen` selects the hash
 * (32=SHA-256, 48=SHA-384, 64=SHA-512). Returns 0 if valid. */
int rsa_pss_verify(const uint8_t *n, int nlen,
                   const uint8_t *e, int elen,
                   const uint8_t *sig, int siglen,
                   const uint8_t *hash, int hashlen);
