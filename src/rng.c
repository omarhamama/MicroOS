/*
 * rng.c — the kernel's random byte source for TLS.
 *
 * TLS needs unpredictable bytes for the ephemeral X25519 key and the
 * ClientHello random. We don't have a hardware RNG, so we whisk together
 * what little entropy a bare VM has: the high-resolution cycle counter
 * (cntvct_el0), sampled repeatedly so scheduling jitter stirs in, mixed
 * through SHA-256 with a running counter. Output is fed back into the
 * pool so successive draws differ.
 *
 * HONESTY NOTE: this is demonstrably weak entropy — a determined
 * attacker who can model the VM's timing could narrow the keyspace.
 * It's fine for *showing TLS work*; it is NOT fit to protect real
 * secrets. A real OS seeds a CSPRNG from many hardware sources. This is
 * the one place our "from scratch" TLS is deliberately a teaching toy.
 */

#include "sha256.h"
#include "lib.h"

static uint8_t pool[32];
static uint64_t counter;

static uint64_t cycles(void)
{
    uint64_t c;
    asm volatile("mrs %0, cntvct_el0" : "=r"(c));
    return c;
}

void tls_random_bytes(uint8_t *out, size_t n)
{
    while (n) {
        struct sha256 s;
        sha256_init(&s);
        sha256_update(&s, pool, sizeof pool);
        /* sample the cycle counter several times; the gaps between
         * samples depend on memory/scheduler timing — our entropy. */
        for (int i = 0; i < 8; i++) {
            uint64_t c = cycles();
            sha256_update(&s, &c, sizeof c);
        }
        counter++;
        sha256_update(&s, &counter, sizeof counter);
        uint8_t block[32];
        sha256_final(&s, block);
        memcpy(pool, block, sizeof pool);   /* reseed the pool */

        size_t take = n < 32 ? n : 32;
        memcpy(out, block, take);
        out += take; n -= take;
    }
}
