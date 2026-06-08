#pragma once

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"
#define MQTT_HOST       "192.168.1.50"
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "hoymiles-esp32"

// BLE advertisement name prefix (from inverter: "RMI-XXXXXXXXXXXX")
// Set to the full name of your inverter.
#define BLE_DEVICE_NAME "RMI-AABBCCDDEE12"

// 12-char serial tail: everything after "RMI-"
#define INVERTER_SN     "AABBCCDDEE12"

// MQTT base topic: hoymiles/<sn>/
#define MQTT_BASE_TOPIC "hoymiles/" INVERTER_SN "/"

#define POLL_INTERVAL_MS   30000
#define WIFI_RETRY_MS      10000
#define MQTT_RETRY_MS      10000
#define BLE_RETRY_MS       60000
#define RESPONSE_TIMEOUT_MS 5000
