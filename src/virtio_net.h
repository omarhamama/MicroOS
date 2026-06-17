#pragma once

#include <stdint.h>

#define ETH_MAX_FRAME 1514

int  vnet_init(void);               /* 0 = NIC found and live */
int  vnet_present(void);
const uint8_t *vnet_mac(void);      /* our 6-byte hardware address */

/* Send one Ethernet frame (blocks briefly until the NIC takes it). */
int vnet_send(const void *frame, uint32_t len);

/* Pull one received frame into `buf` (>= ETH_MAX_FRAME bytes).
 * Returns its length, or -1 if nothing is waiting. */
int vnet_recv(void *buf);

uint64_t vnet_rx_count(void);
uint64_t vnet_tx_count(void);
void    *vnet_rx_chan(void);        /* block on this; the ISR wakes it */
