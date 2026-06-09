#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void (*BleRxCallback)(const uint8_t *data, size_t len);

#ifdef __cplusplus
extern "C" {
#endif

void ble_init(void);
// Scans for a device whose advertised name starts with name_prefix. If
// sn_filter is non-NULL and non-empty, additionally requires the serial tail
// (the name after the prefix) to equal sn_filter exactly. On success, copies
// the matched serial tail into sn_out (sn_out_len must be >= 13), connects,
// negotiates MTU, and subscribes to RX notifications.
bool ble_connect(const char *name_prefix, const char *sn_filter,
                 char *sn_out, size_t sn_out_len);
bool ble_is_connected(void);
bool ble_write(const uint8_t *data, size_t len);
void ble_disconnect(void);
void ble_set_rx_callback(BleRxCallback cb);

#ifdef __cplusplus
}
#endif
