#pragma once

// Per-deployment secrets live in secrets.h (gitignored). Copy secrets.example.h
// to secrets.h and fill it in before building. It defines:
//   WIFI_SSID, WIFI_PASSWORD, MQTT_HOST, MQTT_USER, MQTT_PASSWORD
// The inverter serial is auto-discovered at runtime from its RMI- advertisement.
#include "secrets.h"

#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "hoymiles-esp32"

// Inverter BLE advertisement prefix; the 12-char serial tail after this is
// auto-discovered during the scan.
#define BLE_NAME_PREFIX     "RMI-"

// Optional: if several inverters are in BLE range, set this to the exact
// 12-char serial tail to pin one. Empty = connect to the first RMI- device.
#define INVERTER_SN_FILTER  ""

// MQTT topics are <prefix><discovered-serial>/...  built at runtime.
#define MQTT_TOPIC_PREFIX "hoymiles/"

// CommCmd application-layer handshake (V1) identity. The DTU whitelists a
// client by its bleId; an unknown bleId requires the user's BLE PIN once to be
// added to the whitelist. Generated with hiflow_ble.generate_ble_id().
#define BLE_ID   "100000000000000001"

// BLE_PIN comes from secrets.h. Fallback to empty (no PIN) if not defined there.
#ifndef BLE_PIN
#define BLE_PIN  ""
#endif

#define POLL_INTERVAL_MS    30000
#define WIFI_RETRY_MS       10000
#define MQTT_RETRY_MS       10000
#define BLE_RETRY_MS        60000
#define RESPONSE_TIMEOUT_MS 5000
