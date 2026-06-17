/*
 * tls.c — the TLS 1.3 client handshake and record layer.
 *
 * Read tls.h first for the shape of the conversation. The work here is
 * in four parts:
 *
 *   1. KEY SCHEDULE — TLS 1.3 derives a tree of keys from one ECDH
 *      shared secret using HKDF. Each arrow ("Derive-Secret", "Extract")
 *      is a few HMACs. The labels ("c hs traffic", "key", "finished",
 *      ...) are fixed strings from RFC 8446 §7.
 *
 *   2. RECORD LAYER — after ServerHello everything is wrapped in an
 *      AEAD: each record is ChaCha20-Poly1305 over (header as associated
 *      data, content || real-type || padding). The nonce is the static
 *      IV XOR the record sequence number, which both sides count in
 *      lock-step. A failed tag = a tampered record = abort.
 *
 *   3. TRANSCRIPT — a running SHA-256 of every handshake message. The
 *      server proves it holds the certificate's private key by signing
 *      this transcript (CertificateVerify); both sides prove they
 *      derived the same keys by MAC-ing it (Finished).
 *
 *   4. AUTHENTICATION — validate the certificate chain (x509.c) AND
 *      check CertificateVerify. Encryption without authentication just
 *      means you're privately talking to an impostor, so both are
 *      mandatory before we send a single byte of the request.
 *
 * Scope: one cipher suite (TLS_CHACHA20_POLY1305_SHA256), one group
 * (x25519). No session resumption, no 0-RTT, no HelloRetryRequest, no
 * client certificates — the straight-line happy path of a fresh
 * connection, which is what a browser GET needs.
 */

#include "tls.h"
#include "tcp.h"
#include "asn1.h"
#include "x25519.h"
#include "chacha20.h"
#include "sha256.h"
#include "sha512.h"
#include "rsa.h"
#include "p256.h"
#include "x509.h"
#include "mem.h"
#include "timer.h"
#include "kprintf.h"
#include "lib.h"

#ifdef TLS_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, "[tls] " __VA_ARGS__)
static void dbghex(const char*l,const uint8_t*b,int n){fprintf(stderr,"[tls] %s",l);for(int i=0;i<n;i++)fprintf(stderr,"%02x",b[i]);fprintf(stderr,"\n");}
#else
#define DBG(...)
#define dbghex(a,b,c)
#endif

/* Provided by the platform: fill `out` with n random bytes. The kernel
 * seeds this from the cycle counter (see rng.c); on the host test it's
 * /dev/urandom. (Honesty note: the kernel's entropy is weak — good
 * enough to demonstrate TLS, not to guard real secrets.) */
extern void tls_random_bytes(uint8_t *out, size_t n);

#define REC_MAX   (16384 + 256)         /* max TLS record plaintext + slop */
#define RBUF_MAX  (REC_MAX + 8)
#define HS_MAX    16384                 /* reassembled handshake messages   */

struct keypair { uint8_t key[32]; uint8_t iv[12]; };

struct tls_conn {
    /* raw TCP receive framing buffer */
    uint8_t  rb[RBUF_MAX];
    int      rb_len, rb_pos;
    uint8_t  last_hdr[5];               /* header of the record just read (AAD) */

    struct sha256 transcript;           /* running hash of handshake msgs */

    uint8_t  client_priv[32], client_pub[32];

    /* secrets + derived record keys */
    uint8_t  handshake_secret[32];
    uint8_t  c_hs_secret[32], s_hs_secret[32];
    struct keypair c_hs, s_hs, c_ap, s_ap;
    uint64_t read_seq, write_seq;

    int      rx_app, tx_app;            /* 0 = handshake keys, 1 = app keys */
    int      established;

    /* reassembled handshake stream */
    uint8_t  hs[HS_MAX];
    int      hs_len, hs_pos;
    uint8_t  scratch[REC_MAX];          /* decrypt target (keeps stack small) */

    /* transmit scratch — also off the stack (kernel stacks are 16 KB) */
    uint8_t  txpt[REC_MAX];
    uint8_t  txrec[5 + REC_MAX + 16];

    /* STABLE copy of the certificate chain. next_hs() hands out pointers
     * into hs[], which it compacts on the next read — so anything we
     * must keep past one message (the certs, and the leaf's parsed
     * fields) has to be copied out of hs[] into here first. */
    uint8_t  certbuf[HS_MAX];
    int      certbuf_used;
};

static void seterr_(struct tls_info *info, const char *m)
{
    int i = 0;
    while (m[i] && i < (int)sizeof info->err - 1) { info->err[i] = m[i]; i++; }
    info->err[i] = 0;
}

/* ---- little-endian/big-endian buffer writer ------------------------ */
struct wbuf { uint8_t *p; int len, cap; };
static void w8(struct wbuf *w, uint8_t v){ if (w->len < w->cap) w->p[w->len]=v; w->len++; }
static void w16(struct wbuf *w, uint16_t v){ w8(w,v>>8); w8(w,v&0xff); }
static void wbytes(struct wbuf *w, const void *d, int n){ const uint8_t*s=d; for(int i=0;i<n;i++) w8(w,s[i]); }

/* ---- HKDF-Expand-Label and Derive-Secret (RFC 8446 §7.1) ----------- */

static void hkdf_expand_label(const uint8_t secret[32], const char *label,
                              const uint8_t *ctx, int ctxlen,
                              uint8_t *out, int outlen)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    struct wbuf w = { info, 0, sizeof info };
    w16(&w, (uint16_t)outlen);
    int ll = 6 + (int)strlen(label);            /* "tls13 " + label */
    w8(&w, (uint8_t)ll);
    wbytes(&w, "tls13 ", 6);
    wbytes(&w, label, (int)strlen(label));
    w8(&w, (uint8_t)ctxlen);
    if (ctxlen) wbytes(&w, ctx, ctxlen);
    hkdf_expand(secret, 32, info, w.len, out, outlen);
}

static void derive_secret(const uint8_t secret[32], const char *label,
                          const uint8_t thash[32], uint8_t out[32])
{
    hkdf_expand_label(secret, label, thash, 32, out, 32);
}

/* turn a traffic secret into the record protection key + iv */
static void traffic_keys(const uint8_t secret[32], struct keypair *kp)
{
    hkdf_expand_label(secret, "key", 0, 0, kp->key, 32);
    hkdf_expand_label(secret, "iv",  0, 0, kp->iv, 12);
}

static void transcript_hash(struct tls_conn *c, uint8_t out[32])
{
    struct sha256 tmp = c->transcript;          /* clone, don't disturb */
    sha256_final(&tmp, out);
}

/* ---- record framing over TCP --------------------------------------- */

static int rb_ensure(struct tls_conn *c, int need)
{
    while (c->rb_len - c->rb_pos < need) {
        if (c->rb_pos > 0) {                    /* compact to the front */
            memmove(c->rb, c->rb + c->rb_pos, c->rb_len - c->rb_pos);
            c->rb_len -= c->rb_pos; c->rb_pos = 0;
        }
        if (c->rb_len >= (int)sizeof c->rb) return -1;
        int n = tcp_recv(c->rb + c->rb_len, (uint32_t)(sizeof c->rb - c->rb_len),
                         10 * TIMER_HZ);
        if (n <= 0) return -1;
        c->rb_len += n;
    }
    return 0;
}

/* Read one raw TLS record. *body points into c->rb (valid until the next
 * read). The 5-byte header is copied to c->last_hdr for use as AEAD AAD. */
static int read_raw(struct tls_conn *c, int *type, const uint8_t **body, int *blen)
{
    if (rb_ensure(c, 5) < 0) return -1;
    const uint8_t *h = c->rb + c->rb_pos;
    int len = (h[3] << 8) | h[4];
    if (len < 0 || len > REC_MAX) return -1;
    if (rb_ensure(c, 5 + len) < 0) return -1;
    h = c->rb + c->rb_pos;                       /* rb_ensure may have moved it */
    memcpy(c->last_hdr, h, 5);
    *type = h[0];
    *body = h + 5;
    *blen = len;
    c->rb_pos += 5 + len;
    return 0;
}

static void build_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12])
{
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(seq >> (i * 8));
}

/* Decrypt an application_data record into c->scratch; returns the inner
 * length and content type, or -1 on a bad tag. */
static int decrypt_record(struct tls_conn *c, const uint8_t *body, int blen,
                          int *inner_type)
{
    struct keypair *kp = c->rx_app ? &c->s_ap : &c->s_hs;
    int ctlen = blen - 16;
    if (ctlen < 0) return -1;
    uint8_t nonce[12];
    build_nonce(kp->iv, c->read_seq, nonce);
    if (chacha20_poly1305_open(kp->key, nonce, c->last_hdr, 5,
                               body, ctlen, body + ctlen, c->scratch) != 0)
        return -1;
    c->read_seq++;
    int n = ctlen;
    while (n > 0 && c->scratch[n - 1] == 0) n--;  /* strip zero padding */
    if (n == 0) return -1;
    *inner_type = c->scratch[--n];                /* last real byte = type */
    return n;
}

/* Encrypt `data` (inner content type `ct`) into one record and send it.
 * Uses the connection's heap buffers, not the stack. */
static int send_encrypted(struct tls_conn *c, int ct, const uint8_t *data, int len)
{
    struct keypair *kp = c->tx_app ? &c->c_ap : &c->c_hs;
    int plen = len + 1;                          /* + content-type byte */
    int reclen = plen + 16;                       /* + AEAD tag */
    uint8_t *rec = c->txrec;
    rec[0] = 23; rec[1] = 0x03; rec[2] = 0x03;
    rec[3] = (uint8_t)(reclen >> 8); rec[4] = (uint8_t)reclen;

    memcpy(c->txpt, data, len);
    c->txpt[len] = (uint8_t)ct;

    uint8_t nonce[12];
    build_nonce(kp->iv, c->write_seq, nonce);
    chacha20_poly1305_seal(kp->key, nonce, rec, 5, c->txpt, plen, rec + 5, rec + 5 + plen);
    c->write_seq++;
    return tcp_send(rec, (uint32_t)(5 + reclen));
}

/* Pull the next handshake message from the (decrypted) handshake stream,
 * folding it into the transcript hash. *body valid until the next call. */
static int next_hs(struct tls_conn *c, int *mtype, const uint8_t **body, int *blen)
{
    for (;;) {
        if (c->hs_len - c->hs_pos >= 4) {
            const uint8_t *p = c->hs + c->hs_pos;
            int len = (p[1] << 16) | (p[2] << 8) | p[3];
            if (c->hs_len - c->hs_pos >= 4 + len) {
                sha256_update(&c->transcript, p, 4 + len);  /* transcript */
                *mtype = p[0]; *body = p + 4; *blen = len;
                c->hs_pos += 4 + len;
                return 0;
            }
        }
        /* need more: compact and decrypt another record into hs[] */
        if (c->hs_pos > 0) {
            memmove(c->hs, c->hs + c->hs_pos, c->hs_len - c->hs_pos);
            c->hs_len -= c->hs_pos; c->hs_pos = 0;
        }
        int type, ilen, ict; const uint8_t *body;
        if (read_raw(c, &type, &body, &ilen) < 0) return -1;
        if (type == 20) continue;                /* ChangeCipherSpec: ignore */
        if (type == 22) {                        /* plaintext handshake (SH) */
            if (c->hs_len + ilen > HS_MAX) return -1;
            memcpy(c->hs + c->hs_len, body, ilen); c->hs_len += ilen;
            continue;
        }
        if (type != 23) return -1;
        int n = decrypt_record(c, body, ilen, &ict);
        if (n < 0) return -1;
        if (ict == 21) return -1;                /* alert */
        if (ict != 22) continue;                 /* not handshake; skip */
        if (c->hs_len + n > HS_MAX) return -1;
        memcpy(c->hs + c->hs_len, c->scratch, n); c->hs_len += n;
    }
}

/* ---- ClientHello / ServerHello ------------------------------------- */

static void build_client_hello(struct tls_conn *c, const char *host, uint8_t *out, int *outlen)
{
    struct wbuf w = { out, 0, REC_MAX };
    w8(&w, 1);                                   /* handshake type: ClientHello */
    int lenpos = w.len; w8(&w,0); w8(&w,0); w8(&w,0);   /* 3-byte length, patch later */
    int body0 = w.len;

    w16(&w, 0x0303);                             /* legacy_version */
    uint8_t rnd[32]; tls_random_bytes(rnd, 32); wbytes(&w, rnd, 32);
    uint8_t sid[32]; tls_random_bytes(sid, 32);
    w8(&w, 32); wbytes(&w, sid, 32);             /* legacy_session_id */
    w16(&w, 2); w16(&w, 0x1303);                 /* cipher_suites: chacha20 */
    w8(&w, 1); w8(&w, 0);                        /* compression: null */

    int extpos = w.len; w16(&w, 0);              /* extensions length, patch later */
    int ext0 = w.len;

    /* server_name */
    w16(&w, 0x0000);
    { int hn = (int)strlen(host);
      w16(&w, (uint16_t)(hn + 5)); w16(&w, (uint16_t)(hn + 3));
      w8(&w, 0); w16(&w, (uint16_t)hn); wbytes(&w, host, hn); }
    /* supported_groups: x25519 */
    w16(&w, 0x000a); w16(&w, 4); w16(&w, 2); w16(&w, 0x001d);
    /* signature_algorithms */
    { static const uint16_t algs[] = {0x0403,0x0804,0x0805,0x0401,0x0501,0x0601};
      int na = (int)(sizeof algs/sizeof algs[0]);
      w16(&w, 0x000d); w16(&w, (uint16_t)(2 + na*2)); w16(&w, (uint16_t)(na*2));
      for (int i=0;i<na;i++) w16(&w, algs[i]); }
    /* supported_versions: TLS 1.3 */
    w16(&w, 0x002b); w16(&w, 3); w8(&w, 2); w16(&w, 0x0304);
    /* key_share: x25519 public */
    x25519_base(c->client_pub, c->client_priv);
    w16(&w, 0x0033); w16(&w, 38); w16(&w, 36); w16(&w, 0x001d); w16(&w, 32);
    wbytes(&w, c->client_pub, 32);

    int extlen = w.len - ext0;
    out[extpos] = (uint8_t)(extlen >> 8); out[extpos+1] = (uint8_t)extlen;
    int blen = w.len - body0;
    out[lenpos] = (uint8_t)(blen >> 16); out[lenpos+1] = (uint8_t)(blen >> 8); out[lenpos+2] = (uint8_t)blen;
    *outlen = w.len;
}

/* Parse ServerHello (handshake body), extracting the server's x25519
 * public into spub. Returns 0 on success. */
static int parse_server_hello(const uint8_t *b, int len, uint8_t spub[32])
{
    struct der_cur { const uint8_t *p, *end; } d = { b, b + len };
    if (d.end - d.p < 2 + 32 + 1) return -1;
    d.p += 2;                                    /* legacy_version */
    d.p += 32;                                   /* random */
    int sidlen = *d.p++;
    if (d.p + sidlen + 2 + 1 > d.end) return -1;
    d.p += sidlen;
    int cipher = (d.p[0] << 8) | d.p[1]; d.p += 2;
    if (cipher != 0x1303) return -1;             /* not our cipher suite */
    d.p += 1;                                    /* compression */
    if (d.p + 2 > d.end) return -1;
    int extlen = (d.p[0] << 8) | d.p[1]; d.p += 2;
    const uint8_t *ee = d.p + extlen;
    if (ee > d.end) return -1;
    int got_share = 0;
    while (d.p + 4 <= ee) {
        int et = (d.p[0] << 8) | d.p[1];
        int el = (d.p[2] << 8) | d.p[3];
        d.p += 4;
        if (d.p + el > ee) return -1;
        if (et == 0x0033) {                      /* key_share */
            if (el < 4) return -1;
            int grp = (d.p[0] << 8) | d.p[1];
            int kl  = (d.p[2] << 8) | d.p[3];
            if (grp != 0x001d || kl != 32 || el < 4 + 32) return -1;
            memcpy(spub, d.p + 4, 32);
            got_share = 1;
        }
        d.p += el;
    }
    return got_share ? 0 : -1;
}

/* ---- the handshake -------------------------------------------------- */

static const char *sigscheme_name(int s)
{
    switch (s) {
    case 0x0403: return "ecdsa_secp256r1_sha256";
    case 0x0804: return "rsa_pss_rsae_sha256";
    case 0x0805: return "rsa_pss_rsae_sha384";
    default:     return "unsupported";
    }
}

/* Verify CertificateVerify: the server signs the transcript-so-far. */
static int check_cert_verify(const struct x509 *leaf, int scheme,
                             const uint8_t *sig, int siglen,
                             const uint8_t thash[32])
{
    /* The signed content: 64 spaces, a context string, 0x00, then the
     * transcript hash (RFC 8446 §4.4.3). */
    uint8_t content[64 + 33 + 1 + 32];
    int n = 0;
    for (int i = 0; i < 64; i++) content[n++] = 0x20;
    static const char ctx[] = "TLS 1.3, server CertificateVerify";
    memcpy(content + n, ctx, 33); n += 33;
    content[n++] = 0;
    memcpy(content + n, thash, 32); n += 32;

    if (scheme == 0x0403) {                       /* ECDSA P-256 + SHA-256 */
        if (leaf->key_alg != KEY_EC_P256) return -1;
        uint8_t h[32]; sha256(content, n, h);
        /* signature is DER SEQUENCE { r INTEGER, s INTEGER } */
        struct der d = der_span(sig, siglen);
        const uint8_t *sv; size_t sl;
        if (der_expect(&d, DER_SEQUENCE, &sv, &sl) < 0) return -1;
        struct der ints = der_span(sv, sl);
        const uint8_t *rb, *sb; size_t rl, slen;
        if (der_expect(&ints, DER_INTEGER, &rb, &rl) < 0) return -1;
        if (der_expect(&ints, DER_INTEGER, &sb, &slen) < 0) return -1;
        while (rl > 0 && rb[0] == 0) { rb++; rl--; }
        while (slen > 0 && sb[0] == 0) { sb++; slen--; }
        if (rl > 32 || slen > 32) return -1;
        uint8_t r[32], s[32];
        memset(r, 0, 32); memset(s, 0, 32);
        memcpy(r + 32 - rl, rb, rl);
        memcpy(s + 32 - slen, sb, slen);
        return p256_ecdsa_verify(leaf->ec_point + 1, h, 32, r, s);
    }
    if (scheme == 0x0804 || scheme == 0x0805) {   /* RSA-PSS SHA-256/384 */
        if (leaf->key_alg != KEY_RSA) return -1;
        int hlen = (scheme == 0x0804) ? 32 : 48;
        uint8_t h[64];
        if (hlen == 32) sha256(content, n, h); else sha384(content, n, h);
        return rsa_pss_verify(leaf->rsa_n, (int)leaf->rsa_n_len,
                              leaf->rsa_e, (int)leaf->rsa_e_len,
                              sig, siglen, h, hlen);
    }
    return -1;
}

struct tls_conn *tls_connect(const char *host, const uint8_t ip[4],
                             uint16_t port, int64_t now, struct tls_info *info)
{
    memset(info, 0, sizeof *info);
    info->cipher = "TLS_CHACHA20_POLY1305_SHA256";
    info->group  = "x25519";

    struct tls_conn *c = kmalloc(sizeof *c);
    if (!c) { seterr_(info, "out of memory"); return 0; }
    memset(c, 0, sizeof *c);
    sha256_init(&c->transcript);
    tls_random_bytes(c->client_priv, 32);

    if (tcp_connect(ip, port) < 0) { seterr_(info, "TCP connect failed"); kfree(c); return 0; }

    /* 1) ClientHello (plaintext) + ChangeCipherSpec for compatibility */
    int chlen;
    build_client_hello(c, host, c->txpt, &chlen);
    sha256_update(&c->transcript, c->txpt, chlen);  /* transcript += ClientHello */
    c->txrec[0]=22; c->txrec[1]=3; c->txrec[2]=1;
    c->txrec[3]=(uint8_t)(chlen>>8); c->txrec[4]=(uint8_t)chlen;
    memcpy(c->txrec+5, c->txpt, chlen);
    if (tcp_send(c->txrec, 5+chlen) < 0) { seterr_(info,"send failed"); goto fail; }
    { uint8_t ccs[6] = {20,3,3,0,1,1}; tcp_send(ccs, 6); }

    /* 2) ServerHello (plaintext handshake record) */
    { int mtype, blen; const uint8_t *body;
      if (next_hs(c, &mtype, &body, &blen) < 0 || mtype != 2) {
          seterr_(info, "no ServerHello"); goto fail; }
      uint8_t spub[32];
      if (parse_server_hello(body, blen, spub) < 0) {
          seterr_(info, "ServerHello not TLS1.3/x25519/chacha20"); goto fail; }

      /* 3) ECDH + key schedule */
      uint8_t shared[32]; x25519(shared, c->client_priv, spub);
      uint8_t zeros[32]; memset(zeros, 0, 32);
      uint8_t empty_hash[32]; sha256("", 0, empty_hash);
      uint8_t early[32], derived[32];
      hkdf_extract(zeros, 32, zeros, 32, early);
      derive_secret(early, "derived", empty_hash, derived);
      hkdf_extract(derived, 32, shared, 32, c->handshake_secret);
      uint8_t th[32]; transcript_hash(c, th);     /* hash(CH..SH) */
      derive_secret(c->handshake_secret, "c hs traffic", th, c->c_hs_secret);
      derive_secret(c->handshake_secret, "s hs traffic", th, c->s_hs_secret);
      traffic_keys(c->c_hs_secret, &c->c_hs);
      traffic_keys(c->s_hs_secret, &c->s_hs);
      c->read_seq = c->write_seq = 0;              /* handshake epoch */
    }

    /* 4) read EncryptedExtensions / Certificate / CertificateVerify / Finished */
    const uint8_t *certs[8]; size_t certlens[8]; int ncerts = 0;
    struct x509 leaf; int have_leaf = 0;
    uint8_t th_cert[32], th_cv[32];
    int sawcv = 0;
    {
      int mtype, blen; const uint8_t *body;
      for (;;) {
        if (next_hs(c, &mtype, &body, &blen) < 0) { seterr_(info,"handshake read error"); goto fail; }
        if (mtype == 8) {                         /* EncryptedExtensions: ignore */
        } else if (mtype == 11) {                 /* Certificate */
            const uint8_t *p = body, *e = body + blen;
            if (p + 1 > e) { seterr_(info,"bad Certificate"); goto fail; }
            int ctxl = *p++; p += ctxl;           /* certificate_request_context */
            if (p + 3 > e) { seterr_(info,"bad Certificate"); goto fail; }
            int listlen = (p[0]<<16)|(p[1]<<8)|p[2]; p += 3;
            const uint8_t *le = p + listlen; if (le > e) le = e;
            c->certbuf_used = 0;
            while (p + 3 <= le && ncerts < 8) {
                int cl = (p[0]<<16)|(p[1]<<8)|p[2]; p += 3;
                if (p + cl > le) break;
                /* COPY into stable storage — `p` aliases hs[], which the
                 * next read will compact out from under us. */
                if (c->certbuf_used + cl > (int)sizeof c->certbuf) break;
                memcpy(c->certbuf + c->certbuf_used, p, cl);
                certs[ncerts] = c->certbuf + c->certbuf_used;
                certlens[ncerts] = cl;
                c->certbuf_used += cl;
                ncerts++;
                p += cl;
                if (p + 2 > le) break;
                int extl = (p[0]<<8)|p[1]; p += 2 + extl; /* per-cert extensions */
            }
            DBG("Certificate: ncerts=%d leaf_len=%d\n", ncerts, (int)certlens[0]);
            if (ncerts == 0 || x509_parse(&leaf, certs[0], certlens[0]) < 0) {
                seterr_(info, "leaf certificate unparseable (P-384 key?)"); goto fail; }
            have_leaf = 1;
            transcript_hash(c, th_cert);          /* hash(CH..Certificate) */
            dbghex("th_cert=", th_cert, 16);
        } else if (mtype == 15) {                 /* CertificateVerify */
            if (!have_leaf) { seterr_(info,"CertificateVerify before Certificate"); goto fail; }
            if (blen < 4) { seterr_(info,"bad CertificateVerify"); goto fail; }
            int scheme = (body[0]<<8)|body[1];
            int slen = (body[2]<<8)|body[3];
            info->sigscheme = sigscheme_name(scheme);
            DBG("CertVerify scheme=0x%04x siglen=%d keyalg=%d\n", scheme, slen, leaf.key_alg);
            if (check_cert_verify(&leaf, scheme, body+4, slen, th_cert) != 0) {
                seterr_(info, "CertificateVerify signature invalid"); goto fail; }
            sawcv = 1;
            transcript_hash(c, th_cv);            /* hash(CH..CertificateVerify) */
        } else if (mtype == 20) {                 /* server Finished */
            if (!sawcv) { seterr_(info,"Finished before CertificateVerify"); goto fail; }
            uint8_t fkey[32], expect[32];
            hkdf_expand_label(c->s_hs_secret, "finished", 0, 0, fkey, 32);
            hmac_sha256(fkey, 32, th_cv, 32, expect);
            if (blen != 32 || ct_memcmp(expect, body, 32) != 0) {
                seterr_(info, "server Finished MAC mismatch"); goto fail; }
            break;                                 /* handshake messages done */
        } else {
            seterr_(info, "unexpected handshake message"); goto fail;
        }
      }
    }

    /* 5) validate the certificate chain to a trusted root */
    if (x509_verify_chain(certs, certlens, ncerts, host, now,
                          info->err, sizeof info->err,
                          info->anchor, sizeof info->anchor) != 0) {
        goto fail;                                 /* info->err already set */
    }
    x509_name_cn(leaf.subject, leaf.subject_len, info->subject, sizeof info->subject);
    x509_name_cn(leaf.issuer,  leaf.issuer_len,  info->issuer,  sizeof info->issuer);

    /* 6) client Finished (still under handshake keys), transcript = CH..SF */
    { uint8_t th_sf[32]; transcript_hash(c, th_sf);
      uint8_t fkey[32], vd[32];
      hkdf_expand_label(c->c_hs_secret, "finished", 0, 0, fkey, 32);
      hmac_sha256(fkey, 32, th_sf, 32, vd);
      uint8_t fin[4 + 32];
      fin[0]=20; fin[1]=0; fin[2]=0; fin[3]=32; memcpy(fin+4, vd, 32);
      if (send_encrypted(c, 22, fin, 36) < 0) { seterr_(info,"send Finished failed"); goto fail; }

      /* 7) application traffic keys (transcript through server Finished) */
      uint8_t derived2[32], master[32], zeros[32]; memset(zeros,0,32);
      uint8_t empty_hash[32]; sha256("", 0, empty_hash);
      derive_secret(c->handshake_secret, "derived", empty_hash, derived2);
      hkdf_extract(derived2, 32, zeros, 32, master);
      uint8_t cap[32], sap[32];
      derive_secret(master, "c ap traffic", th_sf, cap);
      derive_secret(master, "s ap traffic", th_sf, sap);
      traffic_keys(cap, &c->c_ap);
      traffic_keys(sap, &c->s_ap);
    }

    c->tx_app = 1; c->write_seq = 0;               /* switch write to app keys */
    c->rx_app = 1; c->read_seq = 0;                /* and read */
    c->established = 1;
    info->ok = 1;
    return c;

fail:
    tcp_close();
    kfree(c);
    return 0;
}

int tls_write(struct tls_conn *c, const void *buf, int len)
{
    if (!c->established) return -1;
    const uint8_t *p = buf;
    while (len > 0) {
        int chunk = len > 16384 ? 16384 : len;
        if (send_encrypted(c, 23, p, chunk) < 0) return -1;
        p += chunk; len -= chunk;
    }
    return (int)((const uint8_t*)p - (const uint8_t*)buf);
}

int tls_read(struct tls_conn *c, void *buf, int max)
{
    if (!c->established) return -1;
    for (;;) {
        int type, ict, n; const uint8_t *body;
        if (read_raw(c, &type, &body, &n) < 0) return -1;
        if (type == 20) continue;                  /* stray CCS */
        if (type != 23) return -1;
        int len = decrypt_record(c, body, n, &ict);
        if (len < 0) return -1;
        if (ict == 23) {                           /* application data */
            int cp = len < max ? len : max;
            memcpy(buf, c->scratch, cp);
            return cp;
        }
        if (ict == 22) continue;                   /* NewSessionTicket etc: skip */
        if (ict == 21) {                           /* alert */
            /* close_notify (level/desc 1,0 or desc 0) => clean EOF */
            return (len >= 2 && c->scratch[1] == 0) ? 0 : -1;
        }
    }
}

void tls_close(struct tls_conn *c)
{
    if (!c) return;
    tcp_close();
    kfree(c);
}
