#pragma once

// Copy this file to `secrets.h` and fill in your own values:
//     cp src/secrets.example.h src/secrets.h
//
// secrets.h is gitignored and must never be committed. config.h #includes it.

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"

#define MQTT_HOST       "192.168.1.50"

// BLE advertisement name of the inverter, "RMI-XXXXXXXXXXXX".
// Find it with any BLE scanner (e.g. nRF Connect).
#define BLE_DEVICE_NAME "RMI-AABBCCDDEE12"

// The 12 characters after "RMI-". Used for BOTH the MQTT topic prefix and the
// AES key derivation, so it must match the inverter exactly.
#define INVERTER_SN     "AABBCCDDEE12"
