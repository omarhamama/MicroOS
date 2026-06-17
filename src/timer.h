#pragma once

#include <stdint.h>

#define TIMER_HZ  100      /* tick 100 times per second (every 10 ms) */

void     timer_init(void);
void     timer_handle_irq(void);
uint64_t timer_ticks(void);
uint64_t timer_uptime_ms(void);
