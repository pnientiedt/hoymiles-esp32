#pragma once

// Copy this file to `secrets.h` and fill in your own values:
//     cp src/secrets.example.h src/secrets.h
//
// secrets.h is gitignored and must never be committed. config.h #includes it.

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

#define MQTT_HOST       "192.168.1.50"

// The inverter's serial is auto-discovered from its "RMI-" BLE advertisement —
// no device serial needs to be configured here.
