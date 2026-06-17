#pragma once

/*
 * tls.h — a minimal TLS 1.3 client (RFC 8446).
 *
 * This is the tunnel that turns http:// into https://. It performs the
 * full TLS 1.3 handshake over an established TCP connection:
 *
 *   ClientHello  ->         (our X25519 public, SNI, what we support)
 *         <-  ServerHello   (their X25519 public, chosen cipher)
 *      ...derive keys from the shared secret, everything after is encrypted...
 *         <-  {Certificate, CertificateVerify, Finished}
 *   {Finished}  ->
 *   {application data: the HTTP request/response}
 *
 * One cipher suite (TLS_CHACHA20_POLY1305_SHA256), one key exchange
 * (X25519) — the modern, software-friendly pair. The server's identity
 * is proven by validating its certificate chain (x509.c) AND checking
 * that it signed the handshake transcript with that certificate's key
 * (CertificateVerify). Both must pass or tls_connect fails.
 */

#include <stdint.h>
#include <stddef.h>

/* Filled in by tls_connect for the UI / logs. */
struct tls_info {
    int  ok;                /* 1 = certificate chain validated */
    char err[112];          /* failure reason if !ok */
    char subject[80];       /* leaf certificate commonName */
    char issuer[80];        /* leaf's issuer commonName */
    char anchor[64];        /* trusted root the chain terminated at */
    const char *cipher;     /* negotiated cipher suite name */
    const char *group;      /* key-exchange group name */
    const char *sigscheme;  /* how the server signed CertificateVerify */
};

struct tls_conn;            /* opaque; allocated by tls_connect */

/* Handshake with host (already resolved to ip) on the given port.
 * `now` is the current time as YYYYMMDDhhmmss for certificate expiry
 * (0 to skip the date check). Returns a connection on full success
 * (handshake complete AND certificate validated), else NULL with the
 * reason in info->err. */
struct tls_conn *tls_connect(const char *host, const uint8_t ip[4],
                             uint16_t port, int64_t now, struct tls_info *info);

int  tls_write(struct tls_conn *c, const void *buf, int len);
/* Returns decrypted application bytes (>0), 0 on clean close, -1 error. */
int  tls_read(struct tls_conn *c, void *buf, int max);
void tls_close(struct tls_conn *c);
