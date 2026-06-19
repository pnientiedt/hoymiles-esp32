#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <PubSubClient.h>

#ifdef __cplusplus
extern "C" {
#endif

// base_topic: runtime MQTT prefix ending in '/', e.g. "hoymiles/AABBCCDDEE12/".
// out_ports/out_port_count (both nullable): on a successful poll, receive the
// discovered PV port numbers (at most 4) so the caller can zero per-panel
// energy_today while the inverter is offline. Untouched on failure or when NULL.
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16],
                 uint8_t *out_ports, uint8_t *out_port_count);

#ifdef __cplusplus
}
#endif
