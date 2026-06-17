/*
 * x509.c — walk a certificate's DER tree and pull out the fields.
 *
 * This is pure structure-following: the ASN.1 layout of a Certificate
 * is fixed (RFC 5280), so we der_next() through it field by field. The
 * fiddly bits are (1) capturing the *raw* bytes of tbsCertificate and
 * the Names — verification and chain-linking compare those byte-for-
 * byte, so we record cursor positions, not parsed copies — and (2) the
 * optional/tagged fields ([0] version, [3] extensions) which we peek
 * for and skip when absent.
 */

#include "x509.h"
#include "asn1.h"
#include "sha256.h"
#include "sha512.h"
#include "rsa.h"
#include "p256.h"
#include "cacerts.h"
#include "lib.h"

/* --- OID byte strings we recognise (the value, not tag/length) ------ */
static const uint8_t OID_RSA_SHA256[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const uint8_t OID_RSA_SHA384[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c};
static const uint8_t OID_RSA_SHA512[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d};
static const uint8_t OID_RSA_ENC[]    = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
static const uint8_t OID_ECDSA_256[]  = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_384[]  = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const uint8_t OID_EC_PUBKEY[]  = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
static const uint8_t OID_P256[]       = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
static const uint8_t OID_SAN[]        = {0x55,0x1d,0x11};
static const uint8_t OID_BASIC_CONSTR[]= {0x55,0x1d,0x13};

static int oideq(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{ return alen == blen && memcmp(a, b, alen) == 0; }

static int sig_alg_of(const uint8_t *oid, size_t n)
{
    if (oideq(oid,n,OID_RSA_SHA256,sizeof OID_RSA_SHA256)) return SIG_RSA_SHA256;
    if (oideq(oid,n,OID_RSA_SHA384,sizeof OID_RSA_SHA384)) return SIG_RSA_SHA384;
    if (oideq(oid,n,OID_RSA_SHA512,sizeof OID_RSA_SHA512)) return SIG_RSA_SHA512;
    if (oideq(oid,n,OID_ECDSA_256,sizeof OID_ECDSA_256))   return SIG_ECDSA_SHA256;
    if (oideq(oid,n,OID_ECDSA_384,sizeof OID_ECDSA_384))   return SIG_ECDSA_SHA384;
    return SIG_UNKNOWN;
}

/* Parse a UTCTime / GeneralizedTime into a sortable integer
 * YYYYMMDDhhmmss. UTCTime years are 2-digit (>=50 => 19xx, else 20xx). */
static int64_t parse_time(int tag, const uint8_t *v, size_t n)
{
    if (n < 10) return 0;
    int idx = 0, year;
    if (tag == DER_GENTIME) {           /* YYYYMMDD... */
        year = (v[0]-'0')*1000 + (v[1]-'0')*100 + (v[2]-'0')*10 + (v[3]-'0');
        idx = 4;
    } else {                            /* UTCTime: YY... */
        int yy = (v[0]-'0')*10 + (v[1]-'0');
        year = yy >= 50 ? 1900 + yy : 2000 + yy;
        idx = 2;
    }
    int mo = (v[idx]-'0')*10+(v[idx+1]-'0');
    int da = (v[idx+2]-'0')*10+(v[idx+3]-'0');
    int hh = (v[idx+4]-'0')*10+(v[idx+5]-'0');
    int mi = (v[idx+6]-'0')*10+(v[idx+7]-'0');
    int ss = (size_t)(idx+9) <= n ? (v[idx+8]-'0')*10+(v[idx+9]-'0') : 0;
    return ((int64_t)year)*10000000000LL + (int64_t)mo*100000000LL +
           (int64_t)da*1000000LL + hh*10000LL + mi*100LL + ss;
}

static int parse_spki(struct x509 *x, const uint8_t *v, size_t vl)
{
    struct der spki = der_span(v, vl);
    const uint8_t *algv; size_t alglen;
    if (der_expect(&spki, DER_SEQUENCE, &algv, &alglen) < 0) return -1;

    struct der alg = der_span(algv, alglen);
    const uint8_t *oid; size_t oidlen;
    if (der_expect(&alg, DER_OID, &oid, &oidlen) < 0) return -1;

    const uint8_t *pk; size_t pklen;
    if (der_expect(&spki, DER_BITSTRING, &pk, &pklen) < 0 || pklen < 2) return -1;
    pk++; pklen--;                      /* skip the "unused bits" byte (0) */

    if (oideq(oid, oidlen, OID_RSA_ENC, sizeof OID_RSA_ENC)) {
        x->key_alg = KEY_RSA;
        struct der rsa = der_span(pk, pklen);
        const uint8_t *seqv; size_t seql;
        if (der_expect(&rsa, DER_SEQUENCE, &seqv, &seql) < 0) return -1;
        struct der ints = der_span(seqv, seql);
        const uint8_t *n, *e; size_t nl, el;
        if (der_expect(&ints, DER_INTEGER, &n, &nl) < 0) return -1;
        if (der_expect(&ints, DER_INTEGER, &e, &el) < 0) return -1;
        while (nl > 0 && n[0] == 0) { n++; nl--; }   /* strip sign byte */
        x->rsa_n = n; x->rsa_n_len = nl;
        x->rsa_e = e; x->rsa_e_len = el;
        return 0;
    }
    if (oideq(oid, oidlen, OID_EC_PUBKEY, sizeof OID_EC_PUBKEY)) {
        const uint8_t *curve; size_t cl;
        if (der_expect(&alg, DER_OID, &curve, &cl) < 0) return -1;
        if (!oideq(curve, cl, OID_P256, sizeof OID_P256)) return -1;  /* P-256 only */
        if (pklen != 65 || pk[0] != 0x04) return -1; /* uncompressed point */
        x->key_alg = KEY_EC_P256;
        memcpy(x->ec_point, pk, 65);
        x->ec_point_len = 65;
        return 0;
    }
    return -1;                          /* unsupported key type */
}

static void parse_extensions(struct x509 *x, const uint8_t *v, size_t vl)
{
    /* v is the content of [3]; inside is a SEQUENCE OF Extension. */
    struct der wrap = der_span(v, vl);
    const uint8_t *listv; size_t listl;
    if (der_expect(&wrap, DER_SEQUENCE, &listv, &listl) < 0) return;

    struct der list = der_span(listv, listl);
    int tag; const uint8_t *ev; size_t el;
    while (der_next(&list, &tag, &ev, &el) == 0) {
        if (tag != DER_SEQUENCE) continue;
        struct der ext = der_span(ev, el);
        const uint8_t *oid; size_t oidlen;
        if (der_expect(&ext, DER_OID, &oid, &oidlen) < 0) continue;

        const uint8_t *val; size_t vlen; int t2;
        if (der_next(&ext, &t2, &val, &vlen) < 0) continue;
        if (t2 == DER_BOOLEAN) {        /* optional 'critical' flag */
            if (der_next(&ext, &t2, &val, &vlen) < 0) continue;
        }
        if (t2 != DER_OCTETSTRING) continue;

        if (oideq(oid, oidlen, OID_SAN, sizeof OID_SAN)) {
            /* val = DER of SubjectAltName ::= SEQUENCE OF GeneralName */
            struct der sd = der_span(val, vlen);
            const uint8_t *sv; size_t sl;
            if (der_expect(&sd, DER_SEQUENCE, &sv, &sl) == 0) {
                x->san = sv; x->san_len = sl;
            }
        } else if (oideq(oid, oidlen, OID_BASIC_CONSTR, sizeof OID_BASIC_CONSTR)) {
            struct der bd = der_span(val, vlen);
            const uint8_t *bv; size_t bl;
            if (der_expect(&bd, DER_SEQUENCE, &bv, &bl) == 0) {
                struct der inner = der_span(bv, bl);
                const uint8_t *cav; size_t cal; int ct;
                if (der_next(&inner, &ct, &cav, &cal) == 0 &&
                    ct == DER_BOOLEAN && cal == 1 && cav[0] != 0)
                    x->is_ca = 1;
            }
        }
    }
}

int x509_parse(struct x509 *x, const uint8_t *der, size_t len)
{
    memset(x, 0, sizeof *x);
    x->raw = der; x->raw_len = len;

    struct der top = der_span(der, len);
    const uint8_t *cv; size_t cl;
    if (der_expect(&top, DER_SEQUENCE, &cv, &cl) < 0) return -1;   /* Certificate */
    struct der cert = der_span(cv, cl);

    /* tbsCertificate — capture its full TLV span (the signed bytes). */
    const uint8_t *tbs_start = cert.p;
    const uint8_t *tv; size_t tl;
    if (der_expect(&cert, DER_SEQUENCE, &tv, &tl) < 0) return -1;
    x->tbs = tbs_start; x->tbs_len = (size_t)(cert.p - tbs_start);

    /* signatureAlgorithm */
    const uint8_t *sav; size_t sal;
    if (der_expect(&cert, DER_SEQUENCE, &sav, &sal) < 0) return -1;
    { struct der a = der_span(sav, sal); const uint8_t *o; size_t ol;
      if (der_expect(&a, DER_OID, &o, &ol) < 0) return -1;
      x->sig_alg = sig_alg_of(o, ol); }

    /* signatureValue BIT STRING (skip the unused-bits byte) */
    const uint8_t *sg; size_t sgl;
    if (der_expect(&cert, DER_BITSTRING, &sg, &sgl) < 0 || sgl < 2) return -1;
    x->sig = sg + 1; x->sig_len = sgl - 1;

    /* ---- inside tbsCertificate ---- */
    struct der tbs = der_span(tv, tl);
    int tag; const uint8_t *v; size_t vl;

    const uint8_t *save = tbs.p;
    if (der_next(&tbs, &tag, &v, &vl) < 0) return -1;
    if (tag != 0xA0) tbs.p = save;          /* no [0] version present */

    if (der_expect(&tbs, DER_INTEGER, &v, &vl) < 0) return -1;   /* serial */
    if (der_expect(&tbs, DER_SEQUENCE, &v, &vl) < 0) return -1;  /* inner sigAlg */

    const uint8_t *iss = tbs.p;
    if (der_expect(&tbs, DER_SEQUENCE, &v, &vl) < 0) return -1;  /* issuer */
    x->issuer = iss; x->issuer_len = (size_t)(tbs.p - iss);

    if (der_expect(&tbs, DER_SEQUENCE, &v, &vl) < 0) return -1;  /* validity */
    { struct der val = der_span(v, vl);
      int t1, t2; const uint8_t *nb, *na; size_t nbl, nal;
      if (der_next(&val, &t1, &nb, &nbl) < 0) return -1;
      if (der_next(&val, &t2, &na, &nal) < 0) return -1;
      x->not_before = parse_time(t1, nb, nbl);
      x->not_after  = parse_time(t2, na, nal); }

    const uint8_t *subj = tbs.p;
    if (der_expect(&tbs, DER_SEQUENCE, &v, &vl) < 0) return -1;  /* subject */
    x->subject = subj; x->subject_len = (size_t)(tbs.p - subj);

    if (der_expect(&tbs, DER_SEQUENCE, &v, &vl) < 0) return -1;  /* SPKI */
    if (parse_spki(x, v, vl) < 0) return -1;

    /* optional [1] issuerUID, [2] subjectUID, then [3] extensions */
    while (der_next(&tbs, &tag, &v, &vl) == 0) {
        if (tag == 0xA3) { parse_extensions(x, v, vl); break; }
    }
    return 0;
}

/* Case-insensitive compare of `host` against a SAN pattern that may
 * start with "*." (matching exactly one left-most label). */
static int host_match(const char *pat, size_t patlen, const char *host)
{
    if (patlen >= 2 && pat[0] == '*' && pat[1] == '.') {
        /* match the part of host after its first label */
        const char *dot = host;
        while (*dot && *dot != '.') dot++;
        if (*dot != '.') return 0;
        const char *rest = dot + 1;
        const char *p = pat + 2;
        size_t pl = patlen - 2;
        if (strlen(rest) != pl) return 0;
        for (size_t i = 0; i < pl; i++)
            if ((rest[i]|32) != (p[i]|32)) return 0;
        return 1;
    }
    if (strlen(host) != patlen) return 0;
    for (size_t i = 0; i < patlen; i++)
        if ((host[i]|32) != (pat[i]|32)) return 0;
    return 1;
}

int x509_matches_host(const struct x509 *c, const char *host)
{
    if (!c->san) return 0;
    struct der d = der_span(c->san, c->san_len);
    int tag; const uint8_t *v; size_t vl;
    while (der_next(&d, &tag, &v, &vl) == 0) {
        if (tag == 0x82)                /* [2] dNSName, IMPLICIT IA5String */
            if (host_match((const char *)v, vl, host))
                return 1;
    }
    return 0;
}

/* ---- signature verification + chain building ---------------------- */

/* Hash the signed region (tbsCertificate) with the algorithm named in
 * the certificate's signatureAlgorithm. */
static int hash_tbs(const struct x509 *c, uint8_t *out, int *outlen)
{
    switch (c->sig_alg) {
    case SIG_RSA_SHA256: case SIG_ECDSA_SHA256:
        sha256(c->tbs, c->tbs_len, out); *outlen = 32; return 0;
    case SIG_RSA_SHA384: case SIG_ECDSA_SHA384:
        sha384(c->tbs, c->tbs_len, out); *outlen = 48; return 0;
    case SIG_RSA_SHA512:
        sha512(c->tbs, c->tbs_len, out); *outlen = 64; return 0;
    }
    return -1;
}

/* Right-align a DER INTEGER (sign byte stripped) into a 32-byte field. */
static int pad32(const uint8_t *v, size_t n, uint8_t out[32])
{
    while (n > 0 && v[0] == 0) { v++; n--; }
    if (n > 32) return -1;
    memset(out, 0, 32);
    memcpy(out + 32 - n, v, n);
    return 0;
}

int x509_check_sig(const struct x509 *cert, const struct x509 *issuer)
{
    uint8_t h[64]; int hlen;
    if (hash_tbs(cert, h, &hlen) < 0)
        return -1;

    if (cert->sig_alg == SIG_RSA_SHA256 || cert->sig_alg == SIG_RSA_SHA384 ||
        cert->sig_alg == SIG_RSA_SHA512) {
        if (issuer->key_alg != KEY_RSA) return -1;
        const uint8_t *prefix = hlen == 32 ? RSA_SHA256_PREFIX
                              : hlen == 48 ? RSA_SHA384_PREFIX
                                           : RSA_SHA512_PREFIX;
        return rsa_verify(issuer->rsa_n, (int)issuer->rsa_n_len,
                          issuer->rsa_e, (int)issuer->rsa_e_len,
                          cert->sig, (int)cert->sig_len,
                          h, hlen, prefix, 19);
    }
    if (cert->sig_alg == SIG_ECDSA_SHA256 || cert->sig_alg == SIG_ECDSA_SHA384) {
        if (issuer->key_alg != KEY_EC_P256) return -1;
        /* signatureValue is DER SEQUENCE { r INTEGER, s INTEGER } */
        struct der d = der_span(cert->sig, cert->sig_len);
        const uint8_t *sv; size_t sl;
        if (der_expect(&d, DER_SEQUENCE, &sv, &sl) < 0) return -1;
        struct der ints = der_span(sv, sl);
        const uint8_t *rb, *sb; size_t rl, sln;
        if (der_expect(&ints, DER_INTEGER, &rb, &rl) < 0) return -1;
        if (der_expect(&ints, DER_INTEGER, &sb, &sln) < 0) return -1;
        uint8_t r[32], s[32];
        if (pad32(rb, rl, r) < 0 || pad32(sb, sln, s) < 0) return -1;
        return p256_ecdsa_verify(issuer->ec_point + 1, h, hlen, r, s);
    }
    return -1;                          /* unsupported sig alg (e.g. PSS, P-384) */
}

static int dn_eq(const struct x509 *a_issuer, const struct x509 *b_subject)
{
    return a_issuer->issuer_len == b_subject->subject_len &&
           memcmp(a_issuer->issuer, b_subject->subject, a_issuer->issuer_len) == 0;
}

static void seterr(char *err, int errlen, const char *m)
{
    if (!err || errlen <= 0) return;
    int i = 0;
    while (m[i] && i < errlen - 1) { err[i] = m[i]; i++; }
    err[i] = 0;
}

int x509_verify_chain(const uint8_t *const *certs, const size_t *lens, int ncerts,
                      const char *host, int64_t now, char *err, int errlen,
                      char *anchor, int anchorlen)
{
    struct x509 c[8];
    if (ncerts < 1 || ncerts > 8) { seterr(err, errlen, "bad chain length"); return -1; }
    for (int i = 0; i < ncerts; i++)
        if (x509_parse(&c[i], certs[i], lens[i]) < 0) {
            seterr(err, errlen, "unparseable certificate (unsupported key? P-384?)");
            return -1;
        }

    /* leaf must be valid for the hostname we asked for */
    if (host && !x509_matches_host(&c[0], host)) {
        seterr(err, errlen, "hostname not in certificate");
        return -1;
    }
    /* every presented cert must be inside its validity window */
    if (now) for (int i = 0; i < ncerts; i++)
        if (now < c[i].not_before || now > c[i].not_after) {
            seterr(err, errlen, "certificate expired or not yet valid");
            return -1;
        }

    /* follow issuer links from the leaf up to a trusted root */
    int cur = 0;
    for (int step = 0; step < 8; step++) {
        /* try the embedded roots first: does a trusted root issue cur? */
        for (int r = 0; r < ca_roots_count; r++) {
            struct x509 root;
            if (x509_parse(&root, ca_roots[r].der, ca_roots[r].len) < 0) continue;
            if (dn_eq(&c[cur], &root)) {
                if (x509_check_sig(&c[cur], &root) == 0) {
                    if (anchor) seterr(anchor, anchorlen, ca_roots[r].name);
                    return 0;           /* reached a trusted anchor — success */
                }
            }
        }
        /* otherwise find the next intermediate that issued cur */
        int next = -1;
        for (int j = 0; j < ncerts; j++) {
            if (j == cur) continue;
            if (dn_eq(&c[cur], &c[j]) && c[j].is_ca) { next = j; break; }
        }
        if (next < 0) {
            seterr(err, errlen, "chain does not reach a trusted root");
            return -1;
        }
        if (x509_check_sig(&c[cur], &c[next]) != 0) {
            seterr(err, errlen, "broken signature in chain");
            return -1;
        }
        cur = next;
    }
    seterr(err, errlen, "chain too long");
    return -1;
}

/* A Name is SEQUENCE OF RDN; each RDN is SET OF AttributeTypeAndValue
 * SEQUENCE { type OID, value }. We hunt for type == commonName (2.5.4.3). */
void x509_name_cn(const uint8_t *name, size_t namelen, char *out, int outlen)
{
    static const uint8_t OID_CN[] = {0x55,0x04,0x03};
    out[0] = 0;
    struct der seq = der_span(name, namelen);
    const uint8_t *rdnv; size_t rdnl;
    if (der_expect(&seq, DER_SEQUENCE, &rdnv, &rdnl) < 0) return;   /* RDNSequence */
    struct der rdns = der_span(rdnv, rdnl);
    int tag; const uint8_t *setv; size_t setl;
    while (der_next(&rdns, &tag, &setv, &setl) == 0) {
        if (tag != DER_SET) continue;
        struct der set = der_span(setv, setl);
        const uint8_t *atvv; size_t atvl;
        while (der_expect(&set, DER_SEQUENCE, &atvv, &atvl) == 0) {
            struct der atv = der_span(atvv, atvl);
            const uint8_t *oid; size_t oidlen;
            if (der_expect(&atv, DER_OID, &oid, &oidlen) < 0) continue;
            const uint8_t *val; size_t vlen; int vt;
            if (der_next(&atv, &vt, &val, &vlen) < 0) continue;
            if (oideq(oid, oidlen, OID_CN, sizeof OID_CN)) {
                int n = vlen < (size_t)(outlen - 1) ? (int)vlen : outlen - 1;
                memcpy(out, val, n); out[n] = 0;
                return;
            }
        }
    }
}
