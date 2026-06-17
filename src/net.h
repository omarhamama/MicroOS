#pragma once

#include <stdint.h>

void net_init(void);        /* start the stack (no-op without a NIC) */
void net_info(void);        /* the `net` command */
void net_ping(int count);   /* the `ping` command: ICMP to the gateway */

/* ---- services for the upper layers (UDP, TCP, DNS, DHCP) ----------- */

/* Wire formats shared by every layer (multi-byte fields BIG-endian). */
struct eth_hdr {
    uint8_t  dst[6], src[6];
    uint16_t type;                  /* 0x0806 ARP, 0x0800 IPv4 */
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ver_ihl, tos;
    uint16_t len, id, frag;
    uint8_t  ttl, proto;            /* 1 ICMP, 6 TCP, 17 UDP */
    uint16_t csum;
    uint8_t  src[4], dst[4];
} __attribute__((packed));

uint16_t htons(uint16_t v);

/* The internet checksum, split so TCP can sum a pseudo-header first:
 * accumulate over any number of buffers, finalize once. */
uint32_t csum_add(uint32_t sum, const void *data, uint32_t len);
uint16_t csum_fin(uint32_t sum);

/* Send an IPv4 packet (via the gateway; 255.255.255.255 broadcasts).
 * Resolves ARP on first use. Returns 0 on success. */
int ip_send(uint8_t proto, const uint8_t dst_ip[4],
            const void *payload, uint32_t len);

/* Our configuration — set by DHCP at boot, static fallback. */
const uint8_t *net_ip(void);
const uint8_t *net_gw(void);
const uint8_t *net_dns(void);
void net_set_config(const uint8_t ip[4], const uint8_t gw[4],
                    const uint8_t dns[4]);
int  net_dhcp_done(void);           /* 1 = the lease came from DHCP */
void net_mark_dhcp(void);
void net_pump(void);                /* drain + route waiting frames */
