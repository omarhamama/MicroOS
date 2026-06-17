#pragma once

/*
 * A spinlock — the atom of multicore correctness.
 *
 * With ONE core, masking interrupts makes any code atomic: nothing
 * else can run. With TWO cores that guarantee evaporates — the other
 * core doesn't care about your DAIF bits; it is executing RIGHT NOW.
 * The only defense is hardware-atomic memory operations:
 *
 *   __atomic_exchange(ACQUIRE) compiles to an ldaxr/stlxr pair — a
 *   load-exclusive/store-exclusive loop where the store FAILS if any
 *   other core touched the location in between. Whoever swaps 0->1
 *   owns the lock; everyone else spins (the `yield` hint tells the
 *   CPU "I'm just waiting", which matters on real silicon).
 *
 * The ACQUIRE/RELEASE pairing also orders memory: everything written
 * under the lock is visible to the next core that takes it.
 */

typedef struct {
    volatile unsigned int v;
} spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
    while (__atomic_exchange_n(&l->v, 1u, __ATOMIC_ACQUIRE))
        while (l->v)
            asm volatile("yield");
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->v, 0u, __ATOMIC_RELEASE);
}
