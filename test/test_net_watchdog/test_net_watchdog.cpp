#include <unity.h>
#include <stdint.h>
#include "../../src/net_watchdog.h"

// Matches the firmware config: 120 s reassoc, 300 s restart, 30 s throttle.
static const net_watchdog_cfg_t CFG = { 120000, 300000, 30000 };

void test_healthy_is_none(void) {
    // now == last_healthy -> elapsed 0
    TEST_ASSERT_EQUAL(NET_ACTION_NONE,
        net_watchdog_decide(1000, 1000, 0, &CFG));
}

void test_just_below_reassoc_is_none(void) {
    // elapsed 119999 < 120000
    TEST_ASSERT_EQUAL(NET_ACTION_NONE,
        net_watchdog_decide(119999, 0, 0, &CFG));
}

void test_at_reassoc_threshold_reassociates(void) {
    // elapsed 120000, last_reassoc far in the past -> throttle satisfied
    TEST_ASSERT_EQUAL(NET_ACTION_REASSOCIATE,
        net_watchdog_decide(120000, 0, 0, &CFG));
}

void test_reassoc_throttled_when_recent(void) {
    // elapsed 130000 (past reassoc threshold) but last_reassoc only 10 s ago
    uint32_t now = 130000;
    uint32_t last_reassoc = now - 10000; // since_reassoc 10000 < 30000
    TEST_ASSERT_EQUAL(NET_ACTION_NONE,
        net_watchdog_decide(now, 0, last_reassoc, &CFG));
}

void test_reassoc_allowed_after_throttle(void) {
    uint32_t now = 200000;
    uint32_t last_reassoc = now - 30000; // exactly the interval
    TEST_ASSERT_EQUAL(NET_ACTION_REASSOCIATE,
        net_watchdog_decide(now, 0, last_reassoc, &CFG));
}

void test_at_restart_threshold_restarts(void) {
    // elapsed 300000 -> restart even though reassoc would also be due
    TEST_ASSERT_EQUAL(NET_ACTION_RESTART,
        net_watchdog_decide(300000, 0, 0, &CFG));
}

void test_restart_takes_precedence_over_throttled_reassoc(void) {
    // elapsed past restart; last_reassoc recent (reassoc throttled) -> still RESTART
    uint32_t now = 310000;
    TEST_ASSERT_EQUAL(NET_ACTION_RESTART,
        net_watchdog_decide(now, 0, now - 1000, &CFG));
}

void test_wraparound_safe(void) {
    // last_healthy near UINT32 top; now has wrapped past 0. elapsed = 130000.
    uint32_t last_healthy = 4294900000u;          // ~0xFFFF0AE0
    uint32_t now = last_healthy + 130000u;        // wraps -> 62704
    TEST_ASSERT_EQUAL(NET_ACTION_REASSOCIATE,
        net_watchdog_decide(now, last_healthy, 0, &CFG));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_healthy_is_none);
    RUN_TEST(test_just_below_reassoc_is_none);
    RUN_TEST(test_at_reassoc_threshold_reassociates);
    RUN_TEST(test_reassoc_throttled_when_recent);
    RUN_TEST(test_reassoc_allowed_after_throttle);
    RUN_TEST(test_at_restart_threshold_restarts);
    RUN_TEST(test_restart_takes_precedence_over_throttled_reassoc);
    RUN_TEST(test_wraparound_safe);
    return UNITY_END();
}
