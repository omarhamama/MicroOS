#pragma once

#include <stdint.h>

/* Send a UDP datagram (dst 255.255.255.255 broadcasts). */
int udp_send(const uint8_t dst_ip[4], uint16_t sport, uint16_t dport,
             const void *payload, uint32_t len);

/* One-datagram mailboxes: bind a port, then wait for what arrives.
 * Enough for the request/response protocols we speak (DHCP, DNS). */
int udp_bind(uint16_t port);                    /* 0 ok, -1 no slots */
int udp_recv(uint16_t port, void *buf, uint32_t max,
             uint32_t timeout_ticks);           /* bytes, or -1 timeout */
void udp_unbind(uint16_t port);

/* Called by net.c for every received UDP packet. */
void udp_input(const uint8_t src_ip[4], const uint8_t *seg, uint32_t len);

/* The DHCP client: ask the network who we should be. */
void dhcp_run(void);
