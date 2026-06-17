#pragma once

#include <stdint.h>

void     smp_init(void);            /* wake core 1 via PSCI */
int      smp_core1_online(void);
uint64_t smp_core1_count(void);     /* its forever-incrementing counter */
