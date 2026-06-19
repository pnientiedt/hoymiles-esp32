#include "energy_reset.h"

uint32_t local_day_key(const struct tm *lt) {
    if (lt == NULL) return 0;
    // (year * 366) + yday is distinct per (year, day-of-year): tm_yday is 0..365,
    // so the 366 stride prevents two different years from colliding. +1 keeps a
    // valid day from ever equalling the 0 "unknown" sentinel.
    uint32_t year = (uint32_t)(lt->tm_year + 1900);
    uint32_t yday = (uint32_t)lt->tm_yday;   // 0..365
    return year * 366u + yday + 1u;
}

bool energy_day_changed(uint32_t stored, uint32_t today) {
    if (stored == 0 || today == 0) return false;
    return stored != today;
}
