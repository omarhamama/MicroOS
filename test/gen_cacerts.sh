#!/bin/sh
# Regenerate src/cacerts.c from the DER root certificates in test/roots/.
# Run from the repo root:  sh test/gen_cacerts.sh
set -e
cd "$(dirname "$0")/.."
OUT=src/cacerts.c

emit_array() {   # $1 = der file, $2 = C identifier
    xxd -i < "$1" | sed "s/^/    /"
}

{
cat <<'HDR'
/*
 * cacerts.c — the trusted root certificate store (GENERATED).
 *
 * Regenerate with: sh test/gen_cacerts.sh
 *
 * These are the anchors of trust: a certificate chain is only believed
 * if it terminates at one of these roots. Three are real public roots
 * (so MicroOS can validate real websites); one is a local test root the
 * demo TLS server uses. Each is the root's DER, embedded verbatim —
 * exactly the bytes a browser ships in its trust store.
 */

#include "cacerts.h"

HDR

for r in microos_test_root isrg_x1 digicert_global digicert_g2 gts_r1 amazon1; do
    echo "static const unsigned char ca_${r}[] = {"
    emit_array "test/roots/${r}.der"
    echo "};"
    echo
done

cat <<'TBL'
const struct ca_root ca_roots[] = {
    { "MicroOS Test Root CA",     ca_microos_test_root, sizeof ca_microos_test_root },
    { "ISRG Root X1",             ca_isrg_x1,           sizeof ca_isrg_x1 },
    { "DigiCert Global Root CA",  ca_digicert_global,   sizeof ca_digicert_global },
    { "DigiCert Global Root G2",  ca_digicert_g2,       sizeof ca_digicert_g2 },
    { "GTS Root R1",              ca_gts_r1,            sizeof ca_gts_r1 },
    { "Amazon Root CA 1",         ca_amazon1,           sizeof ca_amazon1 },
};
const int ca_roots_count = (int)(sizeof ca_roots / sizeof ca_roots[0]);
TBL
} > "$OUT"
echo "wrote $OUT ($(wc -l < $OUT) lines)"
