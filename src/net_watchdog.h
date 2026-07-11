#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the connection watchdog wants the caller to do this tick.
typedef enum {
    NET_ACTION_NONE,        // link healthy, or still within grace window
    NET_ACTION_REASSOCIATE, // force a WiFi teardown + reconnect
    NET_ACTION_RESTART      // give up and ESP.restart()
} net_action_t;

typedef struct {
    uint32_t reassoc_after_ms;     // unhealthy this long -> reassociate
    uint32_t restart_after_ms;     // unhealthy this long -> restart (must be > reassoc)
    uint32_t reassoc_interval_ms;  // min spacing between reassociate attempts
} net_watchdog_cfg_t;

// Pure decision. "Healthy" is defined by the caller (MQTT connected) via
// last_healthy_ms. All args are millis()-style; subtraction is wraparound-safe
// for elapsed spans under ~49.7 days. RESTART takes precedence over REASSOCIATE.
net_action_t net_watchdog_decide(uint32_t now_ms,
                                 uint32_t last_healthy_ms,
                                 uint32_t last_reassoc_ms,
                                 const net_watchdog_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
