#pragma once

// Copy this file to `secrets.h` and fill in your own values:
//     cp src/secrets.example.h src/secrets.h
//
// secrets.h is gitignored and must never be committed. config.h #includes it.

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

#define MQTT_HOST       "192.168.1.50"

// MQTT broker credentials. Leave both empty ("") to connect anonymously.
#define MQTT_USER       ""
#define MQTT_PASSWORD   ""

// CommCmd identity for this deployment (both optional — config.h has fallbacks).
//   BLE_ID:  stable unique-ish decimal string this client presents to the DTU.
//            Override to pin a specific identity; otherwise the config.h default
//            is fine for a single device.
//   BLE_PIN: the S-Miles-app BLE PIN, used ONCE to whitelist BLE_ID. Leave empty
//            if the device has no PIN or BLE_ID is already whitelisted.
// #define BLE_ID          "100000000000000001"
#define BLE_PIN         ""

// The inverter's serial is auto-discovered from its "RMI-" BLE advertisement —
// no device serial needs to be configured here.
