/*
 * udp.c — UDP, and the DHCP client that rides on it.
 *
 * UDP is barely a protocol: take IP's "deliver this blob to that
 * machine" and add two port numbers so the blob reaches the right
 * PROGRAM on that machine, plus a length and an optional checksum.
 * Eight bytes of header. No connection, no ordering, no retransmit —
 * datagram in, datagram out, good luck. (TCP exists because "good
 * luck" isn't a delivery guarantee; see tcp.c.)
 *
 * DHCP is the first thing most machines ever say on a network, and
 * it's a beautiful bootstrap problem: you need an IP address to talk,
 * and you're asking the network FOR an IP address. The escape hatch
 * is broadcast — destination 255.255.255.255 reaches everyone without
 * knowing anyone. The handshake is a four-step dance:
 *
 *   DISCOVER  (us, from 0.0.0.0)  "anyone out there? I need an address"
 *   OFFER     (server)            "how about 10.0.2.15? router is X, DNS is Y"
 *   REQUEST   (us)                "yes please, I'll take 10.0.2.15"
 *   ACK       (server)            "it's yours (for a while)"
 *
 * Run `net` after boot: the address marked "leased via DHCP" was
 * negotiated by this file, not hard-coded.
 */

#include "udp.h"
#include "net.h"
#include "virtio_net.h"
#include "task.h"
#include "timer.h"
#include "lib.h"
#include "kprintf.h"

struct udp_hdr {
    uint16_t sport, dport, len, csum;
} __attribute__((packed));

/* ---- tiny socket table ----------------------------------------------- */

#define NSOCK 4
#define MAILBOX_MAX 1024

static struct sock {
    uint16_t port;                  /* 0 = free slot */
    volatile uint32_t have;         /* bytes waiting (0 = empty) */
    uint8_t  data[MAILBOX_MAX];
} socks[NSOCK];

int udp_bind(uint16_t port)
{
    for (int i = 0; i < NSOCK; i++) {
        if (!socks[i].port) {
            socks[i].port = port;
            socks[i].have = 0;
            return 0;
        }
    }
    return -1;
}

void udp_unbind(uint16_t port)
{
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].port == port)
            socks[i].port = 0;
}

void udp_input(const uint8_t src_ip[4], const uint8_t *seg, uint32_t len)
{
    (void)src_ip;
    if (len < sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *uh = (const struct udp_hdr *)seg;
    uint16_t dport = htons(uh->dport);
    uint32_t paylen = htons(uh->len) - sizeof(*uh);
    if (paylen > len - sizeof(*uh))
        paylen = len - sizeof(*uh);

    for (int i = 0; i < NSOCK; i++) {
        if (socks[i].port == dport && !socks[i].have) {
            if (paylen > MAILBOX_MAX)
                paylen = MAILBOX_MAX;
            memcpy(socks[i].data, seg + sizeof(*uh), paylen);
            socks[i].have = paylen;
            task_wakeup(&socks[i]);
            return;
        }
    }
    /* no listener: dropped, exactly as UDP promises */
}

int udp_recv(uint16_t port, void *buf, uint32_t max, uint32_t timeout_ticks)
{
    struct sock *s = 0;
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].port == port)
            s = &socks[i];
    if (!s)
        return -1;

    uint64_t deadline = timer_ticks() + timeout_ticks;
    while (!s->have && timer_ticks() < deadline) {
        net_pump();             /* during boot, before the rx task runs */
        task_sleep(1);
    }
    if (!s->have)
        return -1;

    uint32_t n = s->have > max ? max : s->have;
    memcpy(buf, s->data, n);
    s->have = 0;
    return (int)n;
}

int udp_send(const uint8_t dst_ip[4], uint16_t sport, uint16_t dport,
             const void *payload, uint32_t len)
{
    static uint8_t pkt[MAILBOX_MAX + sizeof(struct udp_hdr)];
    if (len > MAILBOX_MAX)
        return -1;

    struct udp_hdr *uh = (struct udp_hdr *)pkt;
    uh->sport = htons(sport);
    uh->dport = htons(dport);
    uh->len = htons((uint16_t)(sizeof(*uh) + len));
    uh->csum = 0;                   /* optional in IPv4: 0 = none */
    memcpy(pkt + sizeof(*uh), payload, len);

    return ip_send(17, dst_ip, pkt, sizeof(*uh) + len);
}

/* ---- DHCP -------------------------------------------------------------- */

struct bootp {                      /* the fixed part (RFC 951) */
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  legacy[192];           /* sname + file: unused since 1985 */
    uint32_t magic;                 /* 0x63825363: "this is DHCP now" */
    uint8_t  options[64];
} __attribute__((packed));

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

static void dhcp_fill(struct bootp *b, uint8_t msgtype,
                      const uint8_t *req_ip, const uint8_t *server)
{
    static const uint8_t bcast[4] = {255,255,255,255};
    (void)bcast;
    memset(b, 0, sizeof(*b));
    b->op = 1;                      /* BOOTREQUEST */
    b->htype = 1;                   /* Ethernet */
    b->hlen = 6;
    b->xid = 0x4d4f5321;            /* "MOS!" — any nonzero id works */
    memcpy(b->chaddr, vnet_mac(), 6);
    b->magic = htons(0x6382) | ((uint32_t)htons(0x5363) << 16);

    uint8_t *o = b->options;
    *o++ = 53; *o++ = 1; *o++ = msgtype;        /* DHCP message type */
    if (req_ip) {
        *o++ = 50; *o++ = 4;                    /* requested address */
        memcpy(o, req_ip, 4); o += 4;
    }
    if (server) {
        *o++ = 54; *o++ = 4;                    /* server identifier */
        memcpy(o, server, 4); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;  /* want: mask,router,dns */
    *o++ = 255;                                 /* end */
}

/* Walk the options for a given tag; returns pointer to its value. */
static const uint8_t *dhcp_opt(const struct bootp *b, uint32_t len, uint8_t tag)
{
    const uint8_t *o = b->options;
    const uint8_t *end = (const uint8_t *)b + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (*o == tag)
            return o + 2;
        o += 2 + o[1];
    }
    return 0;
}

void dhcp_run(void)
{
    static const uint8_t bcast[4] = {255,255,255,255};
    static struct bootp pkt;        /* too big for a 16 KiB task stack */

    if (udp_bind(68) < 0)
        return;

    /* DISCOVER, then wait for an OFFER */
    dhcp_fill(&pkt, DHCP_DISCOVER, 0, 0);
    udp_send(bcast, 68, 67, &pkt, sizeof(pkt));
    int n = udp_recv(68, &pkt, sizeof(pkt), TIMER_HZ);
    const uint8_t *t;
    if (n < (int)(sizeof(pkt) - sizeof(pkt.options)) ||
        !(t = dhcp_opt(&pkt, (uint32_t)n, 53)) || *t != DHCP_OFFER) {
        kprintf("[net] DHCP: no offer — using static 10.0.2.15\n");
        udp_unbind(68);
        return;
    }

    uint8_t ip[4], gw[4], dns[4], server[4];
    memcpy(ip, pkt.yiaddr, 4);
    memcpy(gw, (t = dhcp_opt(&pkt, (uint32_t)n, 3)) ? t : net_gw(), 4);
    memcpy(dns, (t = dhcp_opt(&pkt, (uint32_t)n, 6)) ? t : net_dns(), 4);
    memcpy(server, (t = dhcp_opt(&pkt, (uint32_t)n, 54)) ? t : gw, 4);

    /* REQUEST it back, wait for the ACK */
    dhcp_fill(&pkt, DHCP_REQUEST, ip, server);
    udp_send(bcast, 68, 67, &pkt, sizeof(pkt));
    n = udp_recv(68, &pkt, sizeof(pkt), TIMER_HZ);
    udp_unbind(68);

    if (n > 0 && (t = dhcp_opt(&pkt, (uint32_t)n, 53)) && *t == DHCP_ACK) {
        net_set_config(ip, gw, dns);
        net_mark_dhcp();
        kprintf("[net] DHCP lease: %d.%d.%d.%d (router %d.%d.%d.%d, "
                "dns %d.%d.%d.%d)\n",
                ip[0], ip[1], ip[2], ip[3], gw[0], gw[1], gw[2], gw[3],
                dns[0], dns[1], dns[2], dns[3]);
    } else {
        kprintf("[net] DHCP: no ack — using static 10.0.2.15\n");
    }
}
