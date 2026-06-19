#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Collision-free equality key for a local calendar day, derived from local
// broken-down time. Not monotonic — only guaranteed distinct for distinct days.
// Returns 0 (the "unknown" sentinel) when lt is NULL.
uint32_t local_day_key(const struct tm *lt);

// True when a daily reset is due: both keys known (non-zero) and different.
bool energy_day_changed(uint32_t stored, uint32_t today);

#ifdef __cplusplus
}
#endif
