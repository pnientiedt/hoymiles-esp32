#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <time.h>
#include "config.h"
#include "ble_client.h"
#include "handshake.h"
#include "poller.h"
#include "energy_reset.h"
#include "net_watchdog.h"

#define WDT_TIMEOUT_S 30
// Must stay well under WDT_TIMEOUT_S so the watchdog is fed during idle waits.
#define WDT_FEED_SLICE_MS 250
#define MQTT_BUFFER_SIZE 512
// Wipe the paired encRand (forcing a full V0 re-pairing) only after this many
// consecutive poll failures — a single transient timeout shouldn't trigger it.
#define POLL_FAIL_THRESHOLD 3
// Re-pair after this many handshakes that fail *with a live BLE link*. The DTU
// drops its pairing when it powers down overnight, so the cached encRand goes
// stale and login is rejected — re-pairing (V0) is the only recovery. Without
// this the bridge retries the dead secret forever and never wakes at sunrise.
#define HANDSHAKE_FAIL_THRESHOLD 2
#define FW_VERSION "esp32-1.0.0"

// NVS (Preferences) — shares the handshake namespace; the serial is cached so
// the bridge can build its MQTT topics and report health BEFORE its first BLE
// contact of a boot (e.g. when the inverter is asleep at sunrise).
#define NVS_NS      "hoymiles"
#define NVS_KEY_SN  "sn"
#define NVS_KEY_EDAY  "eday"      // local-day key of last energy publish (uint32)
#define NVS_KEY_PORTS "pvports"   // discovered PV port numbers (bytes)

static WiFiClient   s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);

static uint16_t s_tid = 1;
static uint8_t  s_enc_rand[16] = {0};
static bool     s_enc_rand_ready = false;
static int      s_poll_failures = 0;
static int      s_handshake_failures = 0;
static char     s_sn[16] = {0};
static char     s_saved_sn[16] = {0};       // serial currently persisted in NVS
static char     s_base_topic[64] = {0};
static char     s_status_topic[80] = {0};
static uint32_t s_energy_day = 0;            // 0 = unknown (no publish yet / unsynced)
static uint8_t  s_pv_ports[MAX_PV_PORTS] = {0};
static uint8_t  s_pv_port_count = 0;
// Coarse BLE/poll state, surfaced over MQTT so a headless device can be diagnosed
// remotely. "searching" is the normal nightly state (inverter radio off).
static const char *s_ble_state = "boot";

// Connection watchdog state. "Healthy" = MQTT connected. s_last_healthy_ms is
// seeded at boot so bring-up gets a full grace window; if MQTT never comes up
// within the restart window, the watchdog reboots a wedged bring-up.
static uint32_t s_last_healthy_ms = 0;
static uint32_t s_last_reassoc_ms = 0;
static uint32_t s_wifi_reconnect_count = 0;   // forced reassociations this boot
static uint32_t s_mqtt_reconnect_count = 0;   // successful MQTT (re)connects this boot
static const net_watchdog_cfg_t s_net_cfg = {
    NET_REASSOC_AFTER_MS, NET_RESTART_AFTER_MS, NET_REASSOC_INTERVAL_MS
};

// ---- NVS serial cache --------------------------------------------------------

static void load_sn_from_nvs(void) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String sn = prefs.getString(NVS_KEY_SN, "");
    prefs.end();
    if (sn.length() > 0 && sn.length() < sizeof(s_sn)) {
        strncpy(s_sn, sn.c_str(), sizeof(s_sn) - 1);
        s_sn[sizeof(s_sn) - 1] = '\0';
        strncpy(s_saved_sn, s_sn, sizeof(s_saved_sn) - 1);
    }
}

static void save_sn_to_nvs(const char *sn) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(NVS_KEY_SN, sn);
    prefs.end();
    strncpy(s_saved_sn, sn, sizeof(s_saved_sn) - 1);
    s_saved_sn[sizeof(s_saved_sn) - 1] = '\0';
}

// ---- NVS energy-reset state --------------------------------------------------

static void load_energy_state_from_nvs(void) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    s_energy_day = prefs.getUInt(NVS_KEY_EDAY, 0);
    // Guard getBytes with isKey: before the first poll caches ports the key is
    // absent, and getBytes would otherwise log a scary "[E] ... NOT_FOUND" every
    // boot. Absent key simply means "no ports cached yet".
    size_t n = prefs.isKey(NVS_KEY_PORTS)
                   ? prefs.getBytes(NVS_KEY_PORTS, s_pv_ports, sizeof(s_pv_ports))
                   : 0;
    prefs.end();
    s_pv_port_count = (n <= MAX_PV_PORTS) ? (uint8_t)n : 0;
}

static void save_energy_day_to_nvs(uint32_t day) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putUInt(NVS_KEY_EDAY, day);
    prefs.end();
    s_energy_day = day;
}

static void save_pv_ports_to_nvs(const uint8_t *ports, uint8_t count) {
    if (count > MAX_PV_PORTS) count = MAX_PV_PORTS;
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putBytes(NVS_KEY_PORTS, ports, count);
    prefs.end();
    memcpy(s_pv_ports, ports, count);
    s_pv_port_count = count;
}

// ---- MQTT topics -------------------------------------------------------------

static void build_topics(const char *sn) {
    snprintf(s_base_topic, sizeof(s_base_topic), "%s%s/", MQTT_TOPIC_PREFIX, sn);
    snprintf(s_status_topic, sizeof(s_status_topic), "%sstatus", s_base_topic);
}

// Ensure MQTT topics exist even before the inverter's serial is discovered over
// BLE: prefer the NVS-cached serial, then a chip-ID fallback so a fresh flash can
// still report health. The real serial replaces it once the inverter is seen.
static void ensure_topics(void) {
    if (s_base_topic[0] != '\0') return;
    if (s_sn[0] == '\0') {
        uint64_t mac = ESP.getEfuseMac();
        snprintf(s_sn, sizeof(s_sn), "esp32-%06llX",
                 (unsigned long long)(mac & 0xFFFFFF));
    }
    build_topics(s_sn);
}

// ---- Diagnostics -------------------------------------------------------------

static const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// Publish device health under <base>/diag/. Retained, so the broker always holds
// the latest snapshot — crucially, reset_reason persists the cause of the last
// reboot (brownout vs. watchdog vs. clean power-on) for after-the-fact diagnosis.
static void publish_diag(void) {
    if (!s_mqtt.connected() || s_base_topic[0] == '\0') return;
    char topic[112];
    char val[32];

    snprintf(topic, sizeof(topic), "%sdiag/reset_reason", s_base_topic);
    s_mqtt.publish(topic, reset_reason_str(), true);

    snprintf(topic, sizeof(topic), "%sdiag/uptime_s", s_base_topic);
    snprintf(val, sizeof(val), "%lu", (unsigned long)(millis() / 1000));
    s_mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%sdiag/free_heap", s_base_topic);
    snprintf(val, sizeof(val), "%u", (unsigned)esp_get_free_heap_size());
    s_mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%sdiag/min_free_heap", s_base_topic);
    snprintf(val, sizeof(val), "%u", (unsigned)esp_get_minimum_free_heap_size());
    s_mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%sdiag/wifi_rssi", s_base_topic);
    snprintf(val, sizeof(val), "%d", (int)WiFi.RSSI());
    s_mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%sdiag/ble_state", s_base_topic);
    s_mqtt.publish(topic, s_ble_state, true);

    snprintf(topic, sizeof(topic), "%sdiag/wifi_reconnect_count", s_base_topic);
    snprintf(val, sizeof(val), "%lu", (unsigned long)s_wifi_reconnect_count);
    s_mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%sdiag/mqtt_reconnect_count", s_base_topic);
    snprintf(val, sizeof(val), "%lu", (unsigned long)s_mqtt_reconnect_count);
    s_mqtt.publish(topic, val, true);

    snprintf(topic, sizeof(topic), "%sdiag/mqtt_disconnect_s", s_base_topic);
    snprintf(val, sizeof(val), "%lu",
             (unsigned long)((millis() - s_last_healthy_ms) / 1000));
    s_mqtt.publish(topic, val, true);
}

// ---- Connectivity ------------------------------------------------------------

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
    // configTzTime applies TZ + tzset + starts SNTP. Epoch remains UTC, so
    // handshake.cpp's gmtime_r time-sync is unaffected; only localtime_r (used by
    // the energy_today reset) now yields local time.
    configTzTime(TIMEZONE, "pool.ntp.org");
    uint32_t ntp_start = millis();
    while (time(nullptr) < 1700000000UL && millis() - ntp_start < 8000) {
        esp_task_wdt_reset();
        delay(200);
    }
    return true;
}

static bool mqtt_connect(void) {
    if (s_status_topic[0] == '\0') return false;
    if (s_mqtt.connected()) return true;
    Serial.printf("[MQTT] Connecting to %s:%d …\n", MQTT_HOST, MQTT_PORT);
    // Empty credentials → connect anonymously (PubSubClient treats a non-null
    // user as "send username", so a placeholder "" would break open brokers).
    const char *mqtt_user = MQTT_USER[0] ? MQTT_USER : nullptr;
    const char *mqtt_pass = MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr;
    if (!s_mqtt.connect(MQTT_CLIENT_ID, mqtt_user, mqtt_pass, s_status_topic, 1, true, "offline")) {
        Serial.printf("[MQTT] Failed, rc=%d\n", s_mqtt.state());
        return false;
    }
    Serial.println("[MQTT] Connected.");
    s_mqtt_reconnect_count++;
    // status=online now means "the bridge is alive and on the broker" — NOT "the
    // inverter is producing". The Last-Will flips it to offline only when the
    // ESP32 itself drops, making offline a true device-down alarm. Whether the
    // inverter is awake is shown by diag/ble_state and the freshness of ac/*.
    s_mqtt.publish(s_status_topic, "online", true);
    char ver_topic[96];
    snprintf(ver_topic, sizeof(ver_topic), "%sfirmware_version", s_base_topic);
    s_mqtt.publish(ver_topic, FW_VERSION, true);
    return true;
}

static bool ble_connect_and_handshake(void) {
    if (!ble_connect(BLE_NAME_PREFIX, INVERTER_SN_FILTER, s_sn, sizeof(s_sn))) return false;
    Serial.printf("[main] Inverter SN: %s\n", s_sn);
    // The discovered serial is authoritative — rebuild topics on it and cache it
    // so the next boot can report under the right serial before BLE comes up.
    build_topics(s_sn);
    if (strcmp(s_sn, s_saved_sn) != 0) save_sn_to_nvs(s_sn);
    if (!handshake_run(s_sn, &s_tid, s_enc_rand)) {
        // We connected over BLE but login/time-sync failed. The usual cause is a
        // stale cached encRand (the DTU forgets its pairing when it powers down
        // for the night). Clear NVS after a couple of tries so the next attempt
        // re-pairs from scratch — unlike poll failures, handshake failures
        // otherwise never trigger a re-pair, so the bridge would retry the dead
        // secret forever and never recover at sunrise. ble_connect failing (the
        // inverter being asleep/unreachable) does NOT reach here, so an asleep
        // inverter never needlessly wipes a good pairing.
        if (++s_handshake_failures >= HANDSHAKE_FAIL_THRESHOLD) {
            Serial.printf("[main] %d handshakes failed with a live BLE link, "
                          "clearing NVS to force re-pairing.\n", s_handshake_failures);
            handshake_clear_nvs();
            s_handshake_failures = 0;
        }
        ble_disconnect();
        return false;
    }
    s_handshake_failures = 0;
    s_enc_rand_ready = true;
    return true;
}

// Publish a retained energy_today=0 (top-level + cached per-panel) when the local
// calendar day has advanced past the day of the last energy publish. One check
// covers both the midnight rollover (loop ticks ~30s even while the inverter
// sleeps) and a stale value after reboot (s_energy_day is loaded from NVS at boot).
static void maybe_reset_energy_today(void) {
    if (!s_mqtt.connected() || s_base_topic[0] == '\0') return;

    time_t now = time(nullptr);
    if (now < 1700000000) return;            // clock not synced — can't trust the day
    struct tm lt;
    localtime_r(&now, &lt);
    uint32_t today = local_day_key(&lt);

    if (!energy_day_changed(s_energy_day, today)) return;

    char topic[112];
    snprintf(topic, sizeof(topic), "%senergy_today", s_base_topic);
    s_mqtt.publish(topic, "0.000", true);
    for (uint8_t i = 0; i < s_pv_port_count; i++) {
        snprintf(topic, sizeof(topic), "%spv/%u/energy_today", s_base_topic,
                 (unsigned)s_pv_ports[i]);
        s_mqtt.publish(topic, "0.000", true);
    }
    Serial.printf("[main] Day rollover (%lu -> %lu): published energy_today=0 "
                  "(%u panels).\n", (unsigned long)s_energy_day,
                  (unsigned long)today, (unsigned)s_pv_port_count);
    save_energy_day_to_nvs(today);
}

void setup(void) {
    Serial.begin(115200);
    Serial.println("[main] Starting Hoymiles ESP32 bridge.");

    // Watchdog: 30s timeout, panic (reset) on expiry. This Arduino-ESP32 core
    // exposes the legacy API esp_task_wdt_init(timeout_seconds, panic) rather
    // than the newer esp_task_wdt_config_t / esp_task_wdt_reconfigure.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    load_sn_from_nvs();
    load_energy_state_from_nvs();

    // WiFi hardening for a weak link: start the STA interface, max the TX power,
    // and let the driver auto-reconnect between our watchdog checks.
    // persistent(false) keeps WiFi-config NVS writes off the hot path.
    //
    // Modem power-save must stay at WIFI_PS_MIN_MODEM: this device runs WiFi and
    // BLE concurrently, and the ESP32 coexistence layer abort()s inside
    // coex_core_enable() (via esp_bt_controller_enable() -> NimBLEDevice::init())
    // if WiFi power-save is WIFI_PS_NONE. WIFI_PS_NONE boot-loops the firmware.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    // Seed the watchdog's health clock so boot gets a full grace window.
    s_last_healthy_ms = millis();

    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    // Fail a dead-link connect() fast (4 s) instead of blocking ~15 s under the
    // 30 s task-WDT.
    s_mqtt.setSocketTimeout(4);
    // Authoritative MQTT buffer control (runtime); PubSubClient reallocates here.
    s_mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    ble_init();
}

// Idle wait that keeps the system live: feeds the task watchdog AND pumps the
// MQTT client so the connection (and its keepalive) survives the wait. A bare
// delay() longer than WDT_TIMEOUT_S trips a panic reset; not pumping MQTT during
// the 60s BLE-retry wait would let the broker drop the link every night.
static void idle_wait(uint32_t ms) {
    uint32_t start = millis();
    while ((millis() - start) < ms) {
        esp_task_wdt_reset();
        s_mqtt.loop();
        // Refresh the watchdog health clock while connected: the long BLE-retry
        // and poll-interval waits happen here, so without this a healthy-but-busy
        // loop could drift toward the reassoc threshold. Keeps mqtt_disconnect_s
        // near-zero in steady state (it still spikes once on reconnect).
        if (s_mqtt.connected()) s_last_healthy_ms = millis();
        delay(WDT_FEED_SLICE_MS);
    }
}

void loop(void) {
    esp_task_wdt_reset();

    // Connection watchdog. MQTT connected == the only trustworthy end-to-end
    // health signal (WiFi.status() lies with zombie associations on a weak link).
    // Update the health clock while healthy, then act on the decision.
    if (s_mqtt.connected()) s_last_healthy_ms = millis();
    switch (net_watchdog_decide(millis(), s_last_healthy_ms,
                                s_last_reassoc_ms, &s_net_cfg)) {
        case NET_ACTION_REASSOCIATE:
            Serial.println("[watchdog] MQTT unhealthy: forcing WiFi reassociation.");
            WiFi.disconnect(true, true);   // drop link + clear config; wifi_connect() re-begins
            s_wifi_reconnect_count++;
            s_last_reassoc_ms = millis();
            break;
        case NET_ACTION_RESTART:
            Serial.println("[watchdog] MQTT unhealthy past restart window: ESP.restart().");
            Serial.flush();
            ESP.restart();
            break;  // unreachable
        case NET_ACTION_NONE:
        default:
            break;
    }

    // 1. WiFi underpins both MQTT and (indirectly) remote visibility.
    if (!wifi_connect()) { s_ble_state = "wifi_down"; idle_wait(WIFI_RETRY_MS); return; }

    // 2. Build topics from a known serial (NVS cache / chip-ID fallback) so MQTT
    //    and diagnostics work even before the inverter's serial is discovered.
    ensure_topics();

    // 3. Keep MQTT up INDEPENDENTLY of BLE. This is the core robustness change:
    //    the bridge stays on the broker and reports health all night while the
    //    inverter sleeps, instead of going dark and only surfacing as a stale
    //    Last-Will. Now status=offline genuinely means the ESP32 is down.
    if (!mqtt_connect()) { idle_wait(MQTT_RETRY_MS); return; }

    // 3a. Zero energy_today at the local-day rollover / on a stale post-reboot
    //     value, independently of whether the inverter is reachable.
    maybe_reset_energy_today();

    // 4. (Re)establish BLE if needed. Failure here is usually just the inverter's
    //    radio being off (night / no PV) — keep MQTT alive and keep reporting.
    if (!ble_is_connected() || !s_enc_rand_ready) {
        if (!ble_connect_and_handshake()) {
            s_ble_state = "searching";
            publish_diag();
            idle_wait(BLE_RETRY_MS);   // pumps MQTT + WDT during the wait
            return;
        }
        s_ble_state = "ready";
    }

    // 5. Poll the inverter and publish live data.
    uint8_t polled_ports[MAX_PV_PORTS];
    uint8_t polled_port_count = 0;
    if (!poller_poll(s_mqtt, s_base_topic, &s_tid, s_enc_rand,
                     polled_ports, &polled_port_count)) {
        s_poll_failures++;
        s_ble_state = "poll_fail";
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
        s_ble_state = "polling";
        // Cache discovered ports (persist only on change) so per-panel topics can
        // be zeroed overnight / after a reboot before the inverter is reachable.
        if (polled_port_count > 0 &&
            (polled_port_count != s_pv_port_count ||
             memcmp(polled_ports, s_pv_ports, polled_port_count) != 0)) {
            save_pv_ports_to_nvs(polled_ports, polled_port_count);
        }
        // Claim today so the rollover zero won't fire after a real reading.
        time_t now = time(nullptr);
        if (now >= 1700000000) {
            struct tm lt;
            localtime_r(&now, &lt);
            uint32_t today = local_day_key(&lt);
            if (today != 0 && today != s_energy_day) save_energy_day_to_nvs(today);
        }
    }

    publish_diag();

    // 6. Idle until the next poll, pumping MQTT + WDT throughout.
    idle_wait(POLL_INTERVAL_MS);
}
