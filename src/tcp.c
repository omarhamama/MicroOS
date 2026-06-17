/*
 * tcp.c — TCP: turning "good luck" into a byte stream.
 *
 * IP delivers packets maybe, in any order, possibly twice. TCP builds
 * the abstraction every application actually wants — a reliable,
 * ordered stream — out of three ideas:
 *
 *   SEQUENCE NUMBERS  every byte in the stream is numbered, so the
 *                     receiver can order segments and spot gaps
 *   ACKNOWLEDGMENTS   "I have everything up to byte N" — cumulative,
 *                     so one lost ACK costs nothing
 *   THE HANDSHAKE     SYN -> SYN|ACK -> ACK: both sides exchange
 *                     starting numbers before any data flows
 *
 * This is a deliberately HAPPY-PATH implementation: one connection,
 * client-only, and — the honest part — NO RETRANSMISSION. If a
 * segment is lost we stall instead of resending. Over QEMU's local
 * link to the slirp router, loss is effectively zero, so wget works
 * reliably; on real networks, retransmit timers (plus congestion
 * control, the other half of TCP's soul) are exactly the exercise
 * this file leaves open. The state machine, numbering and handshakes
 * are the real thing.
 */

#include "tcp.h"
#include "net.h"
#include "task.h"
#include "timer.h"
#include "lib.h"
#include "kprintf.h"

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  off;                   /* header length in words << 4 */
    uint8_t  flags;
    uint16_t win, csum, urg;
} __attribute__((packed));

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

enum { CLOSED, SYN_SENT, ESTABLISHED, CLOSE_WAIT, FIN_SENT };

static struct {
    int      state;
    uint8_t  peer_ip[4];
    uint16_t lport, rport;
    uint32_t snd_nxt;               /* next byte WE will send */
    uint32_t rcv_nxt;               /* next byte we EXPECT from the peer */
} c;

/* Received stream bytes, parked until tcp_recv collects them. */
#define RXBUF (48 * 1024)
static uint8_t  rxbuf[RXBUF];
static volatile uint32_t rx_w, rx_r;

static uint32_t hton32(uint32_t v)
{
    return ((uint32_t)htons((uint16_t)v) << 16) | htons((uint16_t)(v >> 16));
}

/* Every segment carries a checksum over a PSEUDO-HEADER (the IPs and
 * protocol — so a packet delivered to the wrong host fails) plus the
 * TCP header and data. */
static uint16_t tcp_csum(const uint8_t dst[4], const void *seg, uint32_t len)
{
    struct {
        uint8_t src[4], dst[4];
        uint8_t zero, proto;
        uint16_t len;
    } __attribute__((packed)) ph;

    memcpy(ph.src, net_ip(), 4);
    memcpy(ph.dst, dst, 4);
    ph.zero = 0;
    ph.proto = 6;
    ph.len = htons((uint16_t)len);

    return csum_fin(csum_add(csum_add(0, &ph, sizeof(ph)), seg, len));
}

static int send_segment(uint8_t flags, const void *data, uint32_t len,
                        int with_mss)
{
    static uint8_t seg[1460 + sizeof(struct tcp_hdr) + 4];
    struct tcp_hdr *th = (struct tcp_hdr *)seg;
    uint32_t hdrlen = sizeof(*th) + (with_mss ? 4 : 0);

    if (len > 1460)
        return -1;

    memset(th, 0, sizeof(*th));
    th->sport = htons(c.lport);
    th->dport = htons(c.rport);
    th->seq = hton32(c.snd_nxt);
    th->ack = (flags & ACK) ? hton32(c.rcv_nxt) : 0;
    th->off = (uint8_t)((hdrlen / 4) << 4);
    th->flags = flags;
    th->win = htons(32 * 1024);     /* "send me up to 32K unacked" */

    if (with_mss) {                 /* option: max segment size 1400 */
        seg[sizeof(*th) + 0] = 2;
        seg[sizeof(*th) + 1] = 4;
        seg[sizeof(*th) + 2] = 1400 >> 8;
        seg[sizeof(*th) + 3] = 1400 & 0xFF;
    }
    if (len)
        memcpy(seg + hdrlen, data, len);

    th->csum = tcp_csum(c.peer_ip, seg, hdrlen + len);
    return ip_send(6, c.peer_ip, seg, hdrlen + len);
}

void tcp_input(const uint8_t src_ip[4], const uint8_t *seg, uint32_t len)
{
    (void)src_ip;
    if (c.state == CLOSED || len < sizeof(struct tcp_hdr))
        return;
    const struct tcp_hdr *th = (const struct tcp_hdr *)seg;
    if (htons(th->sport) != c.rport || htons(th->dport) != c.lport)
        return;

    uint32_t hdrlen = (uint32_t)(th->off >> 4) * 4;
    if (hdrlen > len)
        return;
    const uint8_t *data = seg + hdrlen;
    uint32_t dlen = len - hdrlen;
    uint32_t seq = hton32(th->seq);

    if (th->flags & RST) {          /* peer slammed the door */
        c.state = CLOSED;
        task_wakeup(&c);
        return;
    }

    switch (c.state) {
    case SYN_SENT:
        if ((th->flags & (SYN | ACK)) == (SYN | ACK)) {
            c.rcv_nxt = seq + 1;    /* their SYN occupies one number */
            c.snd_nxt = hton32(th->ack);
            send_segment(ACK, 0, 0, 0);
            c.state = ESTABLISHED;
            task_wakeup(&c);
        }
        return;

    case ESTABLISHED:
    case FIN_SENT:
        /* In-order data: take it, ACK it. Anything else: re-ACK what
         * we have (so a confused sender learns where we stand) and
         * drop — no reassembly queue in the happy path. */
        if (dlen && seq == c.rcv_nxt) {
            uint32_t space = RXBUF - (rx_w - rx_r);
            uint32_t take = dlen > space ? space : dlen;
            for (uint32_t i = 0; i < take; i++)
                rxbuf[(rx_w + i) % RXBUF] = data[i];
            rx_w += take;
            c.rcv_nxt += take;
            task_wakeup(&c);
        }
        if (th->flags & FIN) {
            if (seq + dlen == c.rcv_nxt) {      /* nothing missing */
                c.rcv_nxt++;                    /* FIN takes a number too */
                c.state = c.state == FIN_SENT ? CLOSED : CLOSE_WAIT;
                task_wakeup(&c);
            }
        }
        if (dlen || (th->flags & FIN))
            send_segment(ACK, 0, 0, 0);
        return;
    }
}

int tcp_connect(const uint8_t ip[4], uint16_t port)
{
    if (c.state != CLOSED)
        return -1;

    memset(&c, 0, sizeof(c));
    rx_w = rx_r = 0;
    memcpy(c.peer_ip, ip, 4);
    c.rport = port;

    /* Ephemeral port + initial sequence number from the cycle counter
     * (predictable ISNs were a real 1990s attack vector — RFC 6528). */
    uint64_t cnt;
    asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    c.lport = (uint16_t)(50000 + (cnt % 10000));
    c.snd_nxt = (uint32_t)cnt;

    c.state = SYN_SENT;
    send_segment(SYN, 0, 0, 1);     /* with the MSS option */
    c.snd_nxt++;                    /* our SYN occupies one number */

    uint64_t deadline = timer_ticks() + 3 * TIMER_HZ;
    while (c.state == SYN_SENT && timer_ticks() < deadline)
        task_sleep(1);

    if (c.state != ESTABLISHED) {
        c.state = CLOSED;
        return -1;
    }
    return 0;
}

int tcp_send(const void *buf, uint32_t len)
{
    const uint8_t *p = buf;
    while (len) {
        if (c.state != ESTABLISHED)
            return -1;
        uint32_t chunk = len > 1400 ? 1400 : len;
        if (send_segment(PSH | ACK, p, chunk, 0) < 0)
            return -1;
        c.snd_nxt += chunk;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

int tcp_recv(void *buf, uint32_t max, uint32_t timeout_ticks)
{
    uint64_t deadline = timer_ticks() + timeout_ticks;
    while (rx_r == rx_w) {
        if (c.state == CLOSE_WAIT || c.state == CLOSED)
            return 0;               /* stream over, buffer drained */
        if (timer_ticks() >= deadline)
            return -1;
        task_sleep(1);
    }

    uint32_t have = rx_w - rx_r;
    uint32_t n = have > max ? max : have;
    uint8_t *out = buf;
    for (uint32_t i = 0; i < n; i++)
        out[i] = rxbuf[(rx_r + i) % RXBUF];
    rx_r += n;
    return (int)n;
}

void tcp_close(void)
{
    if (c.state == ESTABLISHED || c.state == CLOSE_WAIT) {
        send_segment(FIN | ACK, 0, 0, 0);
        c.snd_nxt++;
        c.state = c.state == CLOSE_WAIT ? CLOSED : FIN_SENT;

        /* a short grace period for the close handshake to finish */
        uint64_t deadline = timer_ticks() + TIMER_HZ / 2;
        while (c.state != CLOSED && timer_ticks() < deadline)
            task_sleep(1);
    }
    c.state = CLOSED;
}
