#include <unity.h>
#include <string.h>
#include <time.h>
#include "../../src/energy_reset.h"

// Helper: build a local broken-down time for a given year (full, e.g. 2025)
// and day-of-year (0-based, as tm_yday).
static struct tm make_tm(int year, int yday) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_yday = yday;
    return t;
}

void test_same_day_no_reset(void) {
    struct tm a = make_tm(2026, 100);
    struct tm b = make_tm(2026, 100);
    TEST_ASSERT_EQUAL_UINT32(local_day_key(&a), local_day_key(&b));
    TEST_ASSERT_FALSE(energy_day_changed(local_day_key(&a), local_day_key(&b)));
}

void test_next_day_resets(void) {
    struct tm a = make_tm(2026, 100);
    struct tm b = make_tm(2026, 101);
    TEST_ASSERT_TRUE(energy_day_changed(local_day_key(&a), local_day_key(&b)));
}

void test_year_boundary_resets(void) {
    struct tm dec31 = make_tm(2025, 364);  // Dec 31 (non-leap yday 364)
    struct tm jan01 = make_tm(2026, 0);     // Jan 1
    TEST_ASSERT_TRUE(energy_day_changed(local_day_key(&dec31), local_day_key(&jan01)));
}

void test_same_yday_different_year_distinct(void) {
    // Guards against a naive yday-only key colliding across years.
    struct tm a = make_tm(2025, 50);
    struct tm b = make_tm(2026, 50);
    TEST_ASSERT_NOT_EQUAL(local_day_key(&a), local_day_key(&b));
    TEST_ASSERT_TRUE(energy_day_changed(local_day_key(&a), local_day_key(&b)));
}

void test_unknown_sentinel_never_resets(void) {
    struct tm a = make_tm(2026, 100);
    uint32_t key = local_day_key(&a);
    TEST_ASSERT_EQUAL_UINT32(0, local_day_key(NULL));
    TEST_ASSERT_FALSE(energy_day_changed(0, key));
    TEST_ASSERT_FALSE(energy_day_changed(key, 0));
    TEST_ASSERT_FALSE(energy_day_changed(0, 0));
}

void test_valid_key_is_nonzero(void) {
    struct tm a = make_tm(2026, 0);
    TEST_ASSERT_NOT_EQUAL(0, local_day_key(&a));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_same_day_no_reset);
    RUN_TEST(test_next_day_resets);
    RUN_TEST(test_year_boundary_resets);
    RUN_TEST(test_same_yday_different_year_distinct);
    RUN_TEST(test_unknown_sentinel_never_resets);
    RUN_TEST(test_valid_key_is_nonzero);
    return UNITY_END();
}
