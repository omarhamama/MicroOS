/*
 * net.c — the core of the network stack: Ethernet, ARP, IPv4, ICMP.
 *
 * The internet is a tower of envelopes inside envelopes:
 *
 *   Ethernet frame [ IPv4 packet [ ICMP / UDP [ DHCP, DNS ] / TCP ] ]
 *
 * This file owns the bottom two layers and the post office between
 * them: ip_send() wraps any payload for the wire, and the receive
 * task opens envelopes and hands them up — ICMP handled here, UDP to
 * udp.c, TCP to tcp.c. The NIC itself (virtio_net.c) just moves the
 * outermost envelope.
 *
 * ARP is the glue nobody mentions: IP addresses are routing fiction;
 * the wire only understands MAC addresses. "who has 10.0.2.2?" must
 * be asked (and answered — we reply for our own IP) before the first
 * real packet can leave.
 *
 * Addressing comes from DHCP at boot (udp.c) with a static fallback —
 * we are 10.0.2.15 behind QEMU's built-in "slirp" router, which NATs
 * us to your Mac's real network. That's why wget can reach the
 * actual internet from inside this toy OS.
 */

#include "net.h"
#include "virtio_net.h"
#include "udp.h"
#include "tcp.h"
#include "task.h"
#include "timer.h"
#include "lib.h"
#include "kprintf.h"

/* slirp's standard layout — replaced by a DHCP lease when one lands */
static uint8_t our_ip[4] = { 10, 0, 2, 15 };
static uint8_t gw_ip[4]  = { 10, 0, 2, 2 };
static uint8_t dns_ip[4] = { 10, 0, 2, 3 };
static int dhcp_done;

const uint8_t *net_ip(void)  { return our_ip; }
const uint8_t *net_gw(void)  { return gw_ip; }
const uint8_t *net_dns(void) { return dns_ip; }
int  net_dhcp_done(void)     { return dhcp_done; }
void net_mark_dhcp(void)     { dhcp_done = 1; }

void net_set_config(const uint8_t ip[4], const uint8_t gw[4],
                    const uint8_t dns[4])
{
    memcpy(our_ip, ip, 4);
    memcpy(gw_ip, gw, 4);
    memcpy(dns_ip, dns, 4);
}

/* ---- byte-order and checksum helpers -------------------------------- */

uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

uint32_t csum_add(uint32_t sum, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t csum_fin(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* ---- ARP ------------------------------------------------------------- */

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;                    /* 1 request, 2 reply */
    uint8_t  sha[6], spa[4];
    uint8_t  tha[6], tpa[4];
} __attribute__((packed));

static uint8_t  gw_mac[6];
static volatile int gw_mac_known;

static int memcmp_eq(const void *a, const void *b, int n)
{
    const uint8_t *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static void send_arp(uint16_t op, const uint8_t *target_mac,
                     const uint8_t *target_ip)
{
    static const uint8_t broadcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    struct {
        struct eth_hdr eth;
        struct arp_pkt arp;
    } __attribute__((packed)) f;

    memset(&f, 0, sizeof(f));
    memcpy(f.eth.dst, op == 1 ? broadcast : target_mac, 6);
    memcpy(f.eth.src, vnet_mac(), 6);
    f.eth.type = htons(0x0806);

    f.arp.htype = htons(1);
    f.arp.ptype = htons(0x0800);
    f.arp.hlen = 6;
    f.arp.plen = 4;
    f.arp.op = htons(op);
    memcpy(f.arp.sha, vnet_mac(), 6);
    memcpy(f.arp.spa, our_ip, 4);
    if (op == 2)
        memcpy(f.arp.tha, target_mac, 6);
    memcpy(f.arp.tpa, target_ip, 4);

    vnet_send(&f, sizeof(f));
}

/* Resolve the gateway's MAC, asking if necessary. 0 = known. */
static int resolve_gw(void)
{
    if (gw_mac_known)
        return 0;
    send_arp(1, 0, gw_ip);
    uint64_t deadline = timer_ticks() + TIMER_HZ;
    while (!gw_mac_known && timer_ticks() < deadline)
        task_sleep(1);
    return gw_mac_known ? 0 : -1;
}

/* ---- IPv4 send: the one door for every upper layer ------------------ */

int ip_send(uint8_t proto, const uint8_t dst_ip[4],
            const void *payload, uint32_t len)
{
    static const uint8_t bcast_ip[4]  = {255,255,255,255};
    static const uint8_t bcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

    struct {
        struct eth_hdr eth;
        struct ip_hdr  ip;
        uint8_t        data[ETH_MAX_FRAME - 34];
    } __attribute__((packed)) f;

    if (len > sizeof(f.data))
        return -1;

    int bcast = memcmp_eq(dst_ip, bcast_ip, 4);
    if (!bcast && resolve_gw() < 0)
        return -1;

    memcpy(f.eth.dst, bcast ? bcast_mac : gw_mac, 6);
    memcpy(f.eth.src, vnet_mac(), 6);
    f.eth.type = htons(0x0800);

    static uint16_t ipid = 1;
    memset(&f.ip, 0, sizeof(f.ip));
    f.ip.ver_ihl = 0x45;
    f.ip.len = htons((uint16_t)(sizeof(f.ip) + len));
    f.ip.id = htons(ipid++);
    f.ip.ttl = 64;
    f.ip.proto = proto;
    memcpy(f.ip.src, dhcp_done || !bcast ? our_ip : (const uint8_t *)"\0\0\0\0", 4);
    memcpy(f.ip.dst, dst_ip, 4);
    f.ip.csum = csum_fin(csum_add(0, &f.ip, sizeof(f.ip)));

    memcpy(f.data, payload, len);
    return vnet_send(&f, sizeof(f.eth) + sizeof(f.ip) + len);
}

/* ---- ICMP (ping) ------------------------------------------------------ */

struct icmp_hdr {
    uint8_t  type, code;
    uint16_t csum, id, seq;
} __attribute__((packed));

static volatile uint16_t last_echo_seq;
static volatile uint64_t last_echo_cycles;

static uint64_t cycles_now(void)
{
    uint64_t c;
    asm volatile("mrs %0, cntvct_el0" : "=r"(c));
    return c;
}

static uint64_t cycles_per_us(void)
{
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f / 1000000;
}

static void send_echo(uint16_t seq)
{
    struct {
        struct icmp_hdr icmp;
        char            payload[32];
    } __attribute__((packed)) p;

    memset(&p, 0, sizeof(p));
    p.icmp.type = 8;
    p.icmp.id = htons(0x4242);
    p.icmp.seq = htons(seq);
    memcpy(p.payload, "MicroOS says hello across the wire!", 32);
    p.icmp.csum = csum_fin(csum_add(0, &p, sizeof(p)));

    ip_send(1, gw_ip, &p, sizeof(p));
}

/* ---- receive: open envelopes, route upward --------------------------- */

static void handle_frame(uint8_t *p, int len)
{
    if (len < (int)sizeof(struct eth_hdr))
        return;
    struct eth_hdr *eth = (struct eth_hdr *)p;

    if (eth->type == htons(0x0806) &&
        len >= (int)(sizeof(*eth) + sizeof(struct arp_pkt))) {
        struct arp_pkt *arp = (struct arp_pkt *)(p + sizeof(*eth));
        if (arp->op == htons(1) && memcmp_eq(arp->tpa, our_ip, 4))
            send_arp(2, arp->sha, arp->spa);        /* "that's me!" */
        else if (arp->op == htons(2) && memcmp_eq(arp->spa, gw_ip, 4)) {
            memcpy(gw_mac, arp->sha, 6);
            gw_mac_known = 1;
        }
        return;
    }

    if (eth->type != htons(0x0800) ||
        len < (int)(sizeof(*eth) + sizeof(struct ip_hdr)))
        return;

    struct ip_hdr *ip = (struct ip_hdr *)(p + sizeof(*eth));
    uint8_t *body = (uint8_t *)ip + (ip->ver_ihl & 0xF) * 4;
    int body_len = (int)(uint16_t)((ip->len >> 8 | ip->len << 8))
                 - (ip->ver_ihl & 0xF) * 4;
    if (body_len < 0 || body + body_len > p + len)
        return;

    switch (ip->proto) {
    case 1: {                       /* ICMP */
        struct icmp_hdr *icmp = (struct icmp_hdr *)body;
        if (body_len < (int)sizeof(*icmp))
            return;
        if (icmp->type == 0 && icmp->id == htons(0x4242)) {
            last_echo_seq = htons(icmp->seq);
            last_echo_cycles = cycles_now();
        } else if (icmp->type == 8) {           /* ping US: reply */
            icmp->type = 0;
            icmp->csum = 0;
            icmp->csum = csum_fin(csum_add(0, icmp, (uint32_t)body_len));
            ip_send(1, ip->src, icmp, (uint32_t)body_len);
        }
        return;
    }
    case 17:                        /* UDP -> udp.c */
        udp_input(ip->src, body, (uint32_t)body_len);
        return;
    case 6:                         /* TCP -> tcp.c */
        tcp_input(ip->src, body, (uint32_t)body_len);
        return;
    }
}

/* The receive task: BLOCKED until the NIC's interrupt says a frame
 * arrived. Runs DHCP once at startup — the OS configures itself. */
static void net_task(uint64_t arg)
{
    (void)arg;
    static uint8_t frame[ETH_MAX_FRAME];

    dhcp_run();                     /* udp.c; prints the lease result */

    for (;;) {
        int len;
        while ((len = vnet_recv(frame)) > 0)
            handle_frame(frame, len);

        asm volatile("msr daifset, #2");
        len = vnet_recv(frame);
        if (len > 0) {
            asm volatile("msr daifclr, #2");
            handle_frame(frame, len);
            continue;
        }
        task_block(vnet_rx_chan());
        asm volatile("msr daifclr, #2");
    }
}

/* DHCP (in udp.c) needs to pump frames while it waits for replies,
 * before the main loop above takes over. */
void net_pump(void)
{
    static uint8_t frame[ETH_MAX_FRAME];
    int len;
    while ((len = vnet_recv(frame)) > 0)
        handle_frame(frame, len);
}

/* ---- commands ---------------------------------------------------------- */

void net_ping(int count)
{
    if (!vnet_present()) {
        kprintf("ping: no network card\n");
        return;
    }
    if (resolve_gw() < 0) {
        kprintf("ping: gateway does not answer ARP\n");
        return;
    }

    for (uint16_t seq = 1; seq <= (uint16_t)count; seq++) {
        uint64_t t0 = cycles_now();
        send_echo(seq);

        uint64_t deadline = timer_ticks() + 2 * TIMER_HZ;
        while (last_echo_seq != seq && timer_ticks() < deadline)
            task_sleep(1);

        if (last_echo_seq == seq)
            kprintf("64 bytes from %d.%d.%d.%d: seq=%u time=%lu us\n",
                    gw_ip[0], gw_ip[1], gw_ip[2], gw_ip[3], seq,
                    (last_echo_cycles - t0) / cycles_per_us());
        else
            kprintf("seq=%u timed out\n", seq);

        if (seq < (uint16_t)count)
            task_sleep(TIMER_HZ / 2);
    }
}

void net_info(void)
{
    if (!vnet_present()) {
        kprintf("no network card (run with -netdev user + virtio-net)\n");
        return;
    }
    const uint8_t *m = vnet_mac();
    kprintf("virtio-net NIC\n");
    kprintf("  MAC:     %x:%x:%x:%x:%x:%x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    kprintf("  IP:      %d.%d.%d.%d (%s)\n",
            our_ip[0], our_ip[1], our_ip[2], our_ip[3],
            dhcp_done ? "leased via DHCP" : "static fallback");
    kprintf("  gateway: %d.%d.%d.%d  MAC %s\n",
            gw_ip[0], gw_ip[1], gw_ip[2], gw_ip[3],
            gw_mac_known ? "resolved" : "not resolved yet");
    kprintf("  DNS:     %d.%d.%d.%d\n",
            dns_ip[0], dns_ip[1], dns_ip[2], dns_ip[3]);
    kprintf("  frames:  %lu received, %lu sent\n",
            vnet_rx_count(), vnet_tx_count());
}

void net_init(void)
{
    if (vnet_init() < 0)
        return;
    task_create("net", net_task, 0);
    const uint8_t *m = vnet_mac();
    kprintf("[boot] network: virtio-net, MAC %x:%x:%x:%x:%x:%x\n",
            m[0], m[1], m[2], m[3], m[4], m[5]);
}
