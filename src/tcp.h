#pragma once

#include <stdint.h>

/* A single blocking TCP client connection (one at a time). */
int  tcp_connect(const uint8_t ip[4], uint16_t port);   /* 0 = established */
int  tcp_send(const void *buf, uint32_t len);
int  tcp_recv(void *buf, uint32_t max, uint32_t timeout_ticks);
                                    /* >0 bytes, 0 = peer closed, -1 timeout */
void tcp_close(void);

/* Called by net.c for every received TCP segment. */
void tcp_input(const uint8_t src_ip[4], const uint8_t *seg, uint32_t len);
