#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "config.h"
#include "ble_client.h"
#include "handshake.h"
#include "poller.h"

#define WDT_TIMEOUT_S 30
// Must stay well under WDT_TIMEOUT_S so the watchdog is fed during idle waits.
#define WDT_FEED_SLICE_MS 250
#define MQTT_BUFFER_SIZE 512
// Wipe the paired encRand (forcing a full V0 re-pairing) only after this many
// consecutive poll failures — a single transient timeout shouldn't trigger it.
#define POLL_FAIL_THRESHOLD 3
#define FW_VERSION "esp32-1.0.0"
#define MQTT_STATUS_TOPIC MQTT_BASE_TOPIC "status"

static WiFiClient   s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);

static uint16_t s_tid = 1;
static uint8_t  s_enc_rand[16] = {0};
static bool     s_enc_rand_ready = false;
static int      s_poll_failures = 0;

static bool wifi_connect(void) {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.printf("[WiFi] Connecting to %s …\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_RETRY_MS) {
            Serial.println("[WiFi] Timeout.");
            return false;
        }
        esp_task_wdt_reset();
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org");
    return true;
}

static bool mqtt_connect(void) {
    if (s_mqtt.connected()) return true;
    Serial.printf("[MQTT] Connecting to %s:%d …\n", MQTT_HOST, MQTT_PORT);
    if (!s_mqtt.connect(MQTT_CLIENT_ID, nullptr, nullptr, MQTT_STATUS_TOPIC, 1, true, "offline")) {
        Serial.printf("[MQTT] Failed, rc=%d\n", s_mqtt.state());
        return false;
    }
    Serial.println("[MQTT] Connected.");
    char ver_topic[128];
    snprintf(ver_topic, sizeof(ver_topic), "%sfirmware_version", MQTT_BASE_TOPIC);
    s_mqtt.publish(ver_topic, FW_VERSION, true);
    return true;
}

static bool ble_connect_and_handshake(void) {
    if (!ble_connect(BLE_DEVICE_NAME, nullptr)) return false;
    if (!handshake_run(&s_tid, s_enc_rand)) {
        ble_disconnect();
        return false;
    }
    s_enc_rand_ready = true;
    return true;
}

void setup(void) {
    Serial.begin(115200);
    Serial.println("[main] Starting Hoymiles ESP32 bridge.");

    // Watchdog: 30s timeout, panic (reset) on expiry. This Arduino-ESP32 core
    // exposes the legacy API esp_task_wdt_init(timeout_seconds, panic) rather
    // than the newer esp_task_wdt_config_t / esp_task_wdt_reconfigure.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    s_mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    ble_init();
}

void loop(void) {
    esp_task_wdt_reset();

    if (!wifi_connect()) { delay(WIFI_RETRY_MS); return; }
    if (!mqtt_connect()) { delay(MQTT_RETRY_MS); return; }

    if (!ble_is_connected() || !s_enc_rand_ready) {
        if (!ble_connect_and_handshake()) {
            delay(BLE_RETRY_MS);
            return;
        }
        s_mqtt.publish(MQTT_STATUS_TOPIC, "online", true);
    }

    if (!poller_poll(s_mqtt, &s_tid, s_enc_rand)) {
        s_poll_failures++;
        // Drop the BLE link so the next loop re-handshakes (reloading encRand
        // from NVS — cheap). Only wipe NVS, forcing a full V0 re-pairing, once
        // failures persist; a one-off timeout must not clear the pairing.
        if (s_poll_failures >= POLL_FAIL_THRESHOLD) {
            Serial.printf("[main] %d consecutive poll failures, clearing NVS.\n",
                          s_poll_failures);
            handshake_clear_nvs();
            s_poll_failures = 0;
        }
        s_enc_rand_ready = false;
        ble_disconnect();
    } else {
        s_poll_failures = 0;
    }

    s_mqtt.loop();

    // Slice the poll-interval wait so we keep feeding the watchdog and pumping
    // MQTT. A single delay(POLL_INTERVAL_MS) would equal the WDT timeout and
    // never reset it, tripping a panic reset on every normal idle cycle.
    uint32_t wait_start = millis();
    while ((millis() - wait_start) < POLL_INTERVAL_MS) {
        esp_task_wdt_reset();
        s_mqtt.loop();
        delay(WDT_FEED_SLICE_MS);
    }
}
