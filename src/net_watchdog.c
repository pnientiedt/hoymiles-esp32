#include "net_watchdog.h"

net_action_t net_watchdog_decide(uint32_t now_ms,
                                 uint32_t last_healthy_ms,
                                 uint32_t last_reassoc_ms,
                                 const net_watchdog_cfg_t *cfg) {
    uint32_t elapsed = now_ms - last_healthy_ms;   // wraparound-safe unsigned math
    if (elapsed >= cfg->restart_after_ms) {
        return NET_ACTION_RESTART;
    }
    if (elapsed >= cfg->reassoc_after_ms) {
        uint32_t since_reassoc = now_ms - last_reassoc_ms;
        if (since_reassoc >= cfg->reassoc_interval_ms) {
            return NET_ACTION_REASSOCIATE;
        }
    }
    return NET_ACTION_NONE;
}
