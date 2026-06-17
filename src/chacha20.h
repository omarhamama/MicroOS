#pragma once

/*
 * chacha20.h — ChaCha20-Poly1305, the AEAD that protects every TLS record.
 *
 * AEAD = "Authenticated Encryption with Associated Data". It does two
 * jobs at once: ENCRYPT the payload (ChaCha20, a stream cipher) and
 * AUTHENTICATE it (Poly1305, a one-time MAC), so a tampered record is
 * rejected, not silently mis-decrypted. The "associated data" is extra
 * bytes that are authenticated but not encrypted — TLS puts the record
 * header there.
 *
 * Why ChaCha20 and not AES for a from-scratch kernel? AES wants either
 * hardware (AES-NI, which we don't expose) or big lookup tables that
 * leak via cache timing. ChaCha20 is nothing but 32-bit add / xor /
 * rotate on a 16-word state — constant-time by construction, fast in
 * plain C, no tables. TLS 1.3 lists it as a first-class cipher suite
 * exactly for devices like this.
 */

#include <stdint.h>
#include <stddef.h>

/* AEAD seal/open. key=32 bytes, nonce=12 bytes (per-record in TLS).
 * seal: plaintext -> ciphertext (same length) + 16-byte tag.
 * open: verifies tag, then decrypts; returns 0 on success, -1 if the
 *       tag is wrong (forged/corrupted — output must be discarded). */
void chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, size_t aadlen,
                            const uint8_t *pt, size_t ptlen,
                            uint8_t *ct, uint8_t tag[16]);

int  chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, size_t aadlen,
                            const uint8_t *ct, size_t ctlen,
                            const uint8_t tag[16], uint8_t *pt);

/* Exposed for the test harness / lower layers. */
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);
void chacha20_xor(const uint8_t key[32], uint32_t counter,
                  const uint8_t nonce[12], const uint8_t *in, uint8_t *out,
                  size_t len);
void poly1305_mac(const uint8_t key[32], const uint8_t *msg, size_t len,
                  uint8_t tag[16]);
