/*
 * http.c — a one-shot HTTP/1.0 client over plain TCP *or* TLS, now
 * fluent in the things that make real pages actually arrive:
 *
 *   REDIRECTS  — a 301/302/.../308 with a Location header is followed
 *                (this is how a bare "http://x" reaches "https://x");
 *   gzip       — if the server compresses the body (Content-Encoding),
 *                we inflate it (inflate.c);
 *   chunked    — a Transfer-Encoding: chunked body is reassembled.
 *
 * The core conversation is still just "send a GET, read the reply"; the
 * rest is parsing the reply's status line and headers and massaging the
 * body. https:// is the same exchange inside a TLS tunnel (tls.c).
 */

#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "tls.h"
#include "rtc.h"
#include "inflate.h"
#include "mem.h"
#include "lib.h"
#include "timer.h"
#include "kprintf.h"

#define HTTP_MAX    (256 * 1024)    /* raw response cap */
#define MAX_REDIR   6

static int64_t now_stamp(void)
{
    if (!rtc_present()) return 0;
    struct datetime dt; rtc_datetime(&dt);
    return ((int64_t)dt.year)*10000000000LL + (int64_t)dt.month*100000000LL +
           (int64_t)dt.day*1000000LL + dt.hour*10000LL + dt.min*100LL + dt.sec;
}

static char lower(char c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* case-insensitive header lookup within [resp, hend). Copies the value
 * (trimmed) into out; returns 1 if found. */
static int get_header(const char *resp, int hlen, const char *name,
                      char *out, int outmax)
{
    int nl = (int)strlen(name);
    int i = 0;
    /* skip the status line */
    while (i < hlen && !(resp[i]=='\r' && resp[i+1]=='\n')) i++;
    i += 2;
    while (i < hlen) {
        int ls = i;
        while (i < hlen && resp[i] != '\n') i++;
        int le = i; i++;
        if (le - ls < nl + 1) continue;
        int match = 1;
        for (int k = 0; k < nl; k++)
            if (lower(resp[ls+k]) != lower(name[k])) { match = 0; break; }
        if (match && resp[ls+nl] == ':') {
            int v = ls + nl + 1;
            while (v < le && (resp[v]==' ' || resp[v]=='\t')) v++;
            int o = 0;
            while (v < le && resp[v] != '\r' && o < outmax-1) out[o++] = resp[v++];
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

/* Fetch one request fully into resp (headers+body). Returns total bytes,
 * or -1. Fills info for https. */
static int fetch_once(int https, const char *host, const uint8_t ip[4],
                      uint16_t port, const char *path, char *resp,
                      struct tls_info *info)
{
    char req[440];
    ksprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MicroOS/1.1\r\n"
             "Accept-Language: en-US,en;q=0.9\r\n"   /* our font is Latin: ask for English */
             "Accept-Encoding: gzip\r\nConnection: close\r\n\r\n", path, host);
    int total = 0, n;
    if (https) {
        struct tls_info tmp;                     /* tls_connect needs non-NULL */
        struct tls_conn *c = tls_connect(host, ip, port, now_stamp(),
                                         info ? info : &tmp);
        if (!c) return -1;
        tls_write(c, req, (int)strlen(req));
        while (total < HTTP_MAX-1 && (n = tls_read(c, resp+total, HTTP_MAX-1-total)) > 0)
            total += n;
        tls_close(c);
    } else {
        if (info) memset(info, 0, sizeof *info);
        if (tcp_connect(ip, port) < 0) return -1;
        tcp_send(req, (uint32_t)strlen(req));
        while (total < HTTP_MAX-1 && (n = tcp_recv(resp+total, HTTP_MAX-1-total, 6*TIMER_HZ)) > 0)
            total += n;
        tcp_close();
    }
    return total ? total : -1;
}

/* Reassemble a Transfer-Encoding: chunked body in place. */
static int dechunk(char *body, int len)
{
    int rp = 0, wp = 0;
    while (rp < len) {
        int sz = 0, any = 0;                     /* parse hex chunk size */
        while (rp < len) {
            char c = body[rp];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz*16 + d; any = 1; rp++;
        }
        while (rp < len && body[rp] != '\n') rp++;   /* skip to end of size line */
        rp++;
        if (!any || sz == 0) break;
        if (rp + sz > len) sz = len - rp;
        memmove(body + wp, body + rp, sz);
        wp += sz; rp += sz;
        if (rp+1 < len && body[rp]=='\r') rp += 2;   /* trailing CRLF */
    }
    return wp;
}

int http_get_info(const char *url, char *out, int max, struct tls_info *info)
{
    char *resp = kmalloc(HTTP_MAX);
    if (!resp) return -1;

    char cur[260];
    ksprintf(cur, sizeof cur, "%s", url);

    for (int hop = 0; hop < MAX_REDIR; hop++) {
        int https = 0;
        const char *u = cur;
        if (!strncmp(u, "https://", 8)) { https = 1; u += 8; }
        else if (!strncmp(u, "http://", 7)) u += 7;

        char host[96]; int i = 0;
        while (u[i] && u[i] != '/' && i < (int)sizeof(host)-1) { host[i]=u[i]; i++; }
        host[i] = 0;
        const char *path = (u[i] == '/') ? u + i : "/";

        /* split an optional :port off the host ("example.com:8080") */
        uint16_t port = https ? 443 : 80;
        for (int k = 0; host[k]; k++)
            if (host[k] == ':') {
                int pv = 0;
                for (int j = k+1; host[j]; j++) pv = pv*10 + (host[j]-'0');
                if (pv > 0 && pv < 65536) port = (uint16_t)pv;
                host[k] = 0;                     /* hostname = part before ':' */
                break;
            }

        uint8_t ip[4];
        if (dns_resolve(host, ip) < 0) { kfree(resp); return -1; }

        int total = fetch_once(https, host, ip, port, path, resp, info);
        if (total < 0) { kfree(resp); return -1; }

        /* find end of headers */
        int hend = total;
        for (int j = 0; j + 3 < total; j++)
            if (resp[j]=='\r'&&resp[j+1]=='\n'&&resp[j+2]=='\r'&&resp[j+3]=='\n') { hend = j+4; break; }

        /* status code from "HTTP/1.x CODE ..." */
        int code = 0;
        { int s = 0; while (s < total && resp[s] != ' ') s++; s++;
          while (s < total && resp[s] >= '0' && resp[s] <= '9') code = code*10 + (resp[s++]-'0'); }

        /* follow redirects */
        char loc[260];
        if (code >= 300 && code < 400 && get_header(resp, hend, "location", loc, sizeof loc)) {
            if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) {
                ksprintf(cur, sizeof cur, "%s", loc);
            } else if (loc[0] == '/') {
                ksprintf(cur, sizeof cur, "%s://%s%s", https?"https":"http", host, loc);
            } else {
                ksprintf(cur, sizeof cur, "%s://%s/%s", https?"https":"http", host, loc);
            }
            continue;                            /* fetch the new location */
        }

        /* body = everything after the headers */
        char *body = resp + hend;
        int blen = total - hend;

        char te[40], ce[40];
        if (get_header(resp, hend, "transfer-encoding", te, sizeof te) &&
            strncmp(te, "chunked", 7) == 0)
            blen = dechunk(body, blen);

        int n;
        if (get_header(resp, hend, "content-encoding", ce, sizeof ce) &&
            (strncmp(ce,"gzip",4)==0 || strncmp(ce,"deflate",7)==0)) {
            if (strncmp(ce,"gzip",4)==0)
                n = gzip_inflate((uint8_t*)body, blen, (uint8_t*)out, max-1);
            else {
                n = zlib_inflate((uint8_t*)body, blen, (uint8_t*)out, max-1);
                if (n < 0) n = inflate_raw((uint8_t*)body, blen, (uint8_t*)out, max-1);
            }
            if (n < 0) { kfree(resp); return -1; }
        } else {
            n = blen > max-1 ? max-1 : blen;
            memcpy(out, body, n);
        }
        out[n] = 0;
        kfree(resp);
        return n;
    }
    kfree(resp);
    return -1;                                   /* too many redirects */
}

int http_get(const char *url, char *body, int max)
{
    return http_get_info(url, body, max, 0);
}
