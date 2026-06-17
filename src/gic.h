#pragma once

#include <stdint.h>

void     gic_init(void);
void     gic_enable_interrupt(uint32_t intid);
uint32_t gic_acknowledge(void);          /* which interrupt fired? */
void     gic_end_of_interrupt(uint32_t intid);

#define GIC_SPURIOUS  1023u   /* "nothing actually pending" */
