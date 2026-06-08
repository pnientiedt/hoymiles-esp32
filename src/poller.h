#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <PubSubClient.h>

#ifdef __cplusplus
extern "C" {
#endif

bool poller_poll(PubSubClient &mqtt, uint16_t *tid, const uint8_t enc_rand[16]);

#ifdef __cplusplus
}
#endif
