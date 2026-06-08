#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void (*BleRxCallback)(const uint8_t *data, size_t len);

#ifdef __cplusplus
extern "C" {
#endif

void ble_init(void);
bool ble_connect(const char *device_name, BleRxCallback rx_cb);
bool ble_is_connected(void);
bool ble_write(const uint8_t *data, size_t len);
void ble_disconnect(void);
void ble_set_rx_callback(BleRxCallback cb);

#ifdef __cplusplus
}
#endif
