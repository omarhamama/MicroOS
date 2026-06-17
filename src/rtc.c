/*
 * rtc.c — the wall clock (PL031 real-time clock).
 *
 * The timer (timer.c) counts ticks since boot — perfect for "how long
 * have we been up", useless for "what time is it". For wall-clock time
 * a machine needs a battery-backed RTC that keeps counting while the
 * power is off. QEMU's virt board gives us an ARM PL031, mapped at
 * 0x0901_0000 (the DTB lists it as "arm,pl031"), initialized to the
 * host's clock. Its data register holds one number: SECONDS SINCE
 * 1970-01-01, the Unix epoch — the same count every Unix machine keeps.
 *
 * Turning that one integer into "Fri 2026-06-13 14:52:01" is pure
 * arithmetic, and it's a small classic of its own. Seconds split
 * trivially into h:m:s and a day count. Days into year/month/day is
 * the hard part, because months and leap years are irregular — we use
 * Howard Hinnant's well-known civil-calendar algorithm, which shifts
 * the year to start in March so the leap day lands at the END of the
 * year and the month lengths follow a clean pattern.
 */

#include "rtc.h"

#define RTC_BASE 0x09010000UL
#define RTC_DR   (*(volatile uint32_t *)(RTC_BASE + 0x00))  /* data: epoch secs */
#define RTC_CR   (*(volatile uint32_t *)(RTC_BASE + 0x0C))  /* control */

int rtc_present(void)
{
    /* The PL031 starts disabled-ish; QEMU's data register reads a
     * plausible (large) epoch once running. Treat a zero reading as
     * "no clock" so the GUI can fall back to uptime. */
    return RTC_DR != 0;
}

uint64_t rtc_epoch(void)
{
    return RTC_DR;
}

void rtc_datetime(struct datetime *dt)
{
    uint64_t secs = rtc_epoch();

    dt->sec  = (int)(secs % 60);
    dt->min  = (int)((secs / 60) % 60);
    dt->hour = (int)((secs / 3600) % 24);

    long days = (long)(secs / 86400);   /* whole days since the epoch */

    /* 1970-01-01 was a Thursday; +4 makes Sunday = 0. */
    dt->wday = (int)((days % 7 + 4) % 7);

    /* --- days-since-epoch -> civil (y, m, d), Hinnant's algorithm --- */
    long z = days + 719468;             /* shift epoch to 0000-03-01 */
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);          /* day of era */
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);     /* day of year */
    unsigned long mp = (5 * doy + 2) / 153;     /* month, March = 0 */
    unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    unsigned m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);

    dt->year  = (int)(y + (m <= 2));    /* Jan/Feb belong to the next year */
    dt->month = (int)m;
    dt->day   = (int)d;
}
