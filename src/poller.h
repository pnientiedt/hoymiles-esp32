#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <PubSubClient.h>

#ifdef __cplusplus
extern "C" {
#endif

// base_topic: runtime MQTT prefix ending in '/', e.g. "hoymiles/AABBCCDDEE12/".
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16]);

#ifdef __cplusplus
}
#endif
