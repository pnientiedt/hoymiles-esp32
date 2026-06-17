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

// DTU BLE PIN from the S-Miles app, used once to whitelist this client's bleId.
// Leave empty ("") if the device has no PIN configured.
#define BLE_PIN         ""

// The inverter's serial is auto-discovered from its "RMI-" BLE advertisement —
// no device serial needs to be configured here.
