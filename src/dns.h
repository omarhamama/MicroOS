#pragma once

#include <stdint.h>

/* Resolve a hostname to an IPv4 address. Also accepts dotted-decimal
 * ("93.184.216.34") directly. 0 = resolved into ip_out. */
int dns_resolve(const char *name, uint8_t ip_out[4]);
