#pragma once

#include <stdint.h>
#include "tls.h"

/* Fetch http(s)://host[/path]. Splits the URL, resolves the host (DNS),
 * and for http:// opens port 80 and speaks plain HTTP/1.0; for https://
 * opens port 443 and runs the TLS 1.3 client (tls.c), validating the
 * certificate chain before sending anything. Copies the response BODY
 * into `body`, NUL-terminated, up to max-1 bytes. Returns the body
 * length, or -1 on failure.
 *
 * http_get_info() additionally reports the TLS/certificate details (for
 * https) via *info — pass NULL if you don't care. For http:// it sets
 * info->ok = 0 and leaves the rest blank. */
int http_get(const char *url, char *body, int max);
int http_get_info(const char *url, char *body, int max, struct tls_info *info);
