/*
 * dns.c — the Domain Name System: the internet's phone book.
 *
 * Machines route by number; humans remember names. DNS bridges them
 * with a query/response protocol so simple it usually fits in one UDP
 * datagram each way:
 *
 *   query:  "A record for example.com?"     (A = IPv4 address)
 *   answer: "example.com is 93.184.216.34, cache it for 86400s"
 *
 * Two charming wire details you only learn by implementing it:
 *
 *   - Names aren't dot-separated strings but LENGTH-PREFIXED labels:
 *     "example.com" travels as [7]example[3]com[0].
 *   - Answers usually don't repeat the name; they point back at it
 *     with a COMPRESSION POINTER (two bytes, top bits 11) — a 1983
 *     space optimization every parser since has been obliged to honor.
 *
 * We ask the DNS server DHCP gave us (QEMU's 10.0.2.3, which forwards
 * to your Mac's real resolver — so this reaches the actual internet).
 */

#include "dns.h"
#include "udp.h"
#include "net.h"
#include "timer.h"
#include "lib.h"
#include "kprintf.h"

struct dns_hdr {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} __attribute__((packed));

#define DNS_PORT_LOCAL 49152

/* "10.0.2.2" -> {10,0,2,2}; returns 0 if it parses cleanly. */
static int parse_dotted(const char *s, uint8_t ip[4])
{
    int part = 0, val = -1;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            val = (val < 0 ? 0 : val * 10) + (*s - '0');
            if (val > 255)
                return -1;
        } else if (*s == '.' && val >= 0 && part < 3) {
            ip[part++] = (uint8_t)val;
            val = -1;
        } else {
            return -1;
        }
    }
    if (part != 3 || val < 0)
        return -1;
    ip[3] = (uint8_t)val;
    return 0;
}

int dns_resolve(const char *name, uint8_t ip_out[4])
{
    if (parse_dotted(name, ip_out) == 0)
        return 0;                       /* it was already an address */

    /* ---- build the query ------------------------------------------ */
    uint8_t pkt[512];
    struct dns_hdr *h = (struct dns_hdr *)pkt;
    memset(h, 0, sizeof(*h));
    h->id = htons(0x4D53);              /* "MS" */
    h->flags = htons(0x0100);           /* standard query, RECURSE please */
    h->qdcount = htons(1);

    /* name -> length-prefixed labels */
    uint8_t *q = pkt + sizeof(*h);
    const char *part = name;
    while (*part) {
        const char *dot = part;
        while (*dot && *dot != '.')
            dot++;
        uint64_t n = (uint64_t)(dot - part);
        if (n == 0 || n > 63 || q + n + 6 > pkt + sizeof(pkt))
            return -1;
        *q++ = (uint8_t)n;
        memcpy(q, part, n);
        q += n;
        part = *dot ? dot + 1 : dot;
    }
    *q++ = 0;                           /* root label ends the name */
    *q++ = 0; *q++ = 1;                 /* QTYPE  A    */
    *q++ = 0; *q++ = 1;                 /* QCLASS IN   */

    /* ---- ask, wait, parse ------------------------------------------ */
    if (udp_bind(DNS_PORT_LOCAL) < 0)
        return -1;
    udp_send(net_dns(), DNS_PORT_LOCAL, 53, pkt, (uint32_t)(q - pkt));

    uint8_t resp[512];
    int n = udp_recv(DNS_PORT_LOCAL, resp, sizeof(resp), 3 * TIMER_HZ);
    udp_unbind(DNS_PORT_LOCAL);
    if (n < (int)sizeof(struct dns_hdr))
        return -1;

    struct dns_hdr *rh = (struct dns_hdr *)resp;
    uint16_t answers = htons(rh->ancount);
    if (!answers)
        return -1;

    /* skip the echoed question */
    uint8_t *p = resp + sizeof(*rh), *end = resp + n;
    while (p < end && *p)               /* labels... */
        p += *p + 1;
    p += 1 + 4;                         /* ...root, qtype, qclass */

    /* walk the answers for the first A record */
    while (answers-- && p + 12 <= end) {
        if ((*p & 0xC0) == 0xC0) {      /* compression pointer */
            p += 2;
        } else {
            while (p < end && *p)
                p += *p + 1;
            p += 1;
        }
        if (p + 10 > end)
            return -1;
        uint16_t type  = (uint16_t)(p[0] << 8 | p[1]);
        uint16_t rdlen = (uint16_t)(p[8] << 8 | p[9]);
        p += 10;
        if (type == 1 && rdlen == 4 && p + 4 <= end) {
            memcpy(ip_out, p, 4);
            return 0;
        }
        p += rdlen;                     /* CNAME etc: keep walking */
    }
    return -1;
}
