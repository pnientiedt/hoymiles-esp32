#pragma once

// Per-deployment secrets live in secrets.h (gitignored). Copy secrets.example.h
// to secrets.h and fill it in before building. It defines:
//   WIFI_SSID, WIFI_PASSWORD, MQTT_HOST, BLE_DEVICE_NAME, INVERTER_SN
#include "secrets.h"

#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "hoymiles-esp32"

// Inverter BLE advertisement prefix; the 12-char serial tail after this is
// auto-discovered during the scan.
#define BLE_NAME_PREFIX     "RMI-"

// Optional: if several inverters are in BLE range, set this to the exact
// 12-char serial tail to pin one. Empty = connect to the first RMI- device.
#define INVERTER_SN_FILTER  ""

// MQTT base topic: hoymiles/<sn>/
#define MQTT_BASE_TOPIC "hoymiles/" INVERTER_SN "/"

#define POLL_INTERVAL_MS    30000
#define WIFI_RETRY_MS       10000
#define MQTT_RETRY_MS       10000
#define BLE_RETRY_MS        60000
#define RESPONSE_TIMEOUT_MS 5000
