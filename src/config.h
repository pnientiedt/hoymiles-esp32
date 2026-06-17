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

// CommCmd application-layer handshake (V1) identity. The DTU whitelists a client
// by this bleId; an unknown bleId must be authorised once with the device's BLE
// PIN (see BLE_PIN). Both are per-deployment and belong in secrets.h; the
// fallbacks below let the firmware build out of the box.
//
//   - BLE_ID:  any stable, unique-ish decimal string. The placeholder works for
//              a single device (you'll just authorise it once with the PIN).
//              Override in secrets.h to pin a specific identity.
//   - BLE_PIN: the S-Miles-app BLE PIN; empty = device has no PIN, or the bleId
//              is already whitelisted. Submitted at most once (never on repeat,
//              which would lock the DTU).
#ifndef BLE_ID
#define BLE_ID   "100000000000000001"
#endif
#ifndef BLE_PIN
#define BLE_PIN  ""
#endif

#define POLL_INTERVAL_MS    30000
#define WIFI_RETRY_MS       10000
#define MQTT_RETRY_MS       10000
#define BLE_RETRY_MS        60000
#define RESPONSE_TIMEOUT_MS 5000
