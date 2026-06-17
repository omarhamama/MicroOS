#pragma once

#include <stdint.h>

/* Broken-down calendar time (UTC, or local if QEMU's RTC is local). */
struct datetime {
    int year, month, day;       /* month 1..12, day 1..31 */
    int hour, min, sec;
    int wday;                   /* 0 = Sunday .. 6 = Saturday */
};

uint64_t rtc_epoch(void);                   /* seconds since 1970-01-01 */
void     rtc_datetime(struct datetime *dt); /* now, broken down */
int      rtc_present(void);
