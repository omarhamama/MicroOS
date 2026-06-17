/*
 * smp.c — waking the second CPU core.
 *
 * boot.S has parked every core but 0 since the first instruction. This
 * file un-parks core 1 the official ARM way: a PSCI CPU_ON call to
 * firmware ("start MPIDR 1 at this address with this argument"). The
 * new core arrives at secondary_start with the MMU off, gets a stack
 * via the PSCI context argument, configures its OWN MMU registers
 * (per-core state — the page TABLES are shared, the pointer to them
 * is not), and starts doing genuinely simultaneous work.
 *
 * And now the honest part, which is the real lesson:
 *
 *   THE REST OF THIS KERNEL IS NOT SAFE FOR CORE 1 TO TOUCH.
 *
 * Every "critical section" so far is protected by masking interrupts —
 * which restrains the scheduler on THIS core and does nothing at all
 * about a second core executing concurrently. If core 1 called
 * kmalloc while core 0 was in kfree, the free list would corrupt; the
 * same goes for the scheduler, the filesystem, the virtqueues. That's
 * why core 1 runs a confined counting loop and shares exactly ONE
 * structure: kprintf's output, guarded by exactly one spinlock
 * (spinlock.h). Making the whole kernel SMP-safe means auditing every
 * shared structure and choosing a lock for it — that project is how
 * Linux spent the years 1996-2011 (look up the Big Kernel Lock).
 */

#include "smp.h"
#include "mmu.h"
#include "kprintf.h"

static char core1_stack[8192] __attribute__((aligned(16)));
static volatile int      core1_up;
static volatile uint64_t core1_counter;

extern char secondary_start[];      /* boot.S */
extern char vector_table[];         /* vectors.S */

int smp_core1_online(void)      { return core1_up; }
uint64_t smp_core1_count(void)  { return core1_counter; }

static int psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context)
{
    register uint64_t x0 asm("x0") = 0xC4000003;    /* CPU_ON, 64-bit */
    register uint64_t x1 asm("x1") = mpidr;
    register uint64_t x2 asm("x2") = entry;
    register uint64_t x3 asm("x3") = context;
    asm volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int)(int64_t)x0;
}

/* Core 1 lands here from boot.S (stack already set from the PSCI
 * context argument), MMU off, interrupts masked — and they STAY
 * masked: this core takes no interrupts, runs no scheduler. */
void secondary_main(void)
{
    mmu_enable_this_core();
    asm volatile("msr vbar_el1, %0" :: "r"(vector_table));

    uint64_t mpidr, freq;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    core1_up = 1;
    kprintf("[smp] hello from core %lu — two CPUs, one kernel\n",
            mpidr & 0xFF);

    /* Count once per millisecond, forever. Watch it tick in `cores`
     * or the GUI while core 0 does everything else — that's real
     * parallelism, not time-slicing. */
    for (;;) {
        uint64_t t0, now;
        asm volatile("mrs %0, cntvct_el0" : "=r"(t0));
        do
            asm volatile("mrs %0, cntvct_el0" : "=r"(now));
        while (now - t0 < freq / 1000);
        core1_counter++;
    }
}

void smp_init(void)
{
    kprintf_enable_lock();      /* two writers from here on */

    int rc = psci_cpu_on(1, (uint64_t)secondary_start,
                         (uint64_t)(core1_stack + sizeof(core1_stack)));
    if (rc != 0) {
        kprintf("[boot] core 1: PSCI CPU_ON said %d — uniprocessor it is "
                "(QEMU needs -smp 2)\n", rc);
        return;
    }

    for (volatile int spin = 0; !core1_up && spin < 50000000; spin++) { }
    if (core1_up)
        kprintf("[boot] core 1 online — counting in parallel ('cores')\n");
}
