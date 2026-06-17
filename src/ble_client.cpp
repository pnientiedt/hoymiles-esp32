#include "ble_client.h"
#include "config.h"
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <atomic>
#include <string.h>
#include <esp_task_wdt.h>

#define SVC_UUID  "0000e0ff-3c17-d293-8e48-14fe2e4da212"
#define TX_UUID   "0000ffe1-0000-1000-8000-00805f9b34fb"
#define RX_UUID   "0000ffe2-0000-1000-8000-00805f9b34fb"

static NimBLEClient *s_client = nullptr;
static NimBLERemoteCharacteristic *s_tx = nullptr;
static NimBLERemoteCharacteristic *s_rx = nullptr;
static std::atomic<BleRxCallback> s_rx_cb{nullptr};
static bool s_connected = false;

static void notify_cb(NimBLERemoteCharacteristic *chr,
                      uint8_t *data, size_t len, bool is_notify) {
    // Accept BOTH notifications and indications: bleak's start_notify (the
    // hiflow-ble reference) handles either, and dropping indications here would
    // silently lose the DTU's reply if it indicates instead of notifies.
    Serial.printf("[BLE] RX %s len=%u\n", is_notify ? "notify" : "indicate",
                  (unsigned)len);
    auto cb = s_rx_cb.load();
    if (cb) cb(data, len);
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *client) override {
        s_connected = false;
        Serial.println("[BLE] Disconnected.");
    }
};

static ClientCallbacks s_client_cbs;

void ble_init(void) {
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(512);
}

bool ble_connect(const char *name_prefix, const char *sn_filter,
                 char *sn_out, size_t sn_out_len) {
    if (sn_out == nullptr || sn_out_len == 0) return false;

    // Clean up any pre-existing client before creating a new one
    if (s_client) {
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    NimBLEScanResults results = scan->start(5);
    esp_task_wdt_reset();

    size_t prefix_len = strlen(name_prefix);
    NimBLEAddress target_addr;
    bool found = false;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        std::string name = dev.getName();
        if (name.size() <= prefix_len) continue;
        if (name.compare(0, prefix_len, name_prefix) != 0) continue;
        const char *tail = name.c_str() + prefix_len;
        if (sn_filter && sn_filter[0] != '\0' && strcmp(tail, sn_filter) != 0) continue;
        strncpy(sn_out, tail, sn_out_len - 1);
        sn_out[sn_out_len - 1] = '\0';
        target_addr = dev.getAddress();
        found = true;
        break;
    }
    if (!found) {
        Serial.printf("[BLE] No device matching '%s*' found.\n", name_prefix);
        return false;
    }

    s_client = NimBLEDevice::createClient();
    if (!s_client) {
        Serial.println("[BLE] createClient() failed.");
        return false;
    }
    s_client->setClientCallbacks(&s_client_cbs, false);
    s_client->setConnectionParams(12, 12, 0, 51);
    if (!s_client->connect(target_addr)) {
        Serial.println("[BLE] Connect failed.");
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
        return false;
    }
    esp_task_wdt_reset();

    auto *svc = s_client->getService(SVC_UUID);
    if (!svc) {
        Serial.println("[BLE] Service not found.");
        s_client->disconnect();
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
        return false;
    }

    s_tx = svc->getCharacteristic(TX_UUID);
    s_rx = svc->getCharacteristic(RX_UUID);
    if (!s_tx || !s_rx) {
        Serial.println("[BLE] Characteristic not found.");
        s_client->disconnect();
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
        return false;
    }

    // Diagnostics + adaptive subscribe. subscribe(true)=notifications,
    // subscribe(false)=indications. Pick based on what RX actually supports.
    bool can_notify   = s_rx->canNotify();
    bool can_indicate = s_rx->canIndicate();
    Serial.printf("[BLE] MTU=%u | TX props: write=%d writeNR=%d | RX props: notify=%d indicate=%d\n",
                  (unsigned)s_client->getMTU(),
                  s_tx->canWrite(), s_tx->canWriteNoResponse(),
                  can_notify, can_indicate);
    // RX (0xffe2) advertises both notify and indicate; the working reference
    // (bleak) receives notifications. Subscribe for notify, but force the CCCD
    // descriptor write to be ACKNOWLEDGED (response=true). NimBLE writes the
    // CCCD WITHOUT response by default; if the DTU only arms notifications on an
    // acknowledged CCCD write, the default leaves notifications never actually
    // enabled — explaining ATT-ACK'd data writes but zero RX.
    bool want_notifications = can_notify ? true : false;
    Serial.printf("[BLE] Subscribing for %s (CCCD write-with-response)\n",
                  want_notifications ? "NOTIFY" : "INDICATE");
    if (!s_rx->subscribe(want_notifications, notify_cb, /*response=*/true)) {
        Serial.println("[BLE] Subscribe failed.");
        s_client->disconnect();
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
        return false;
    }

    s_connected = true;
    Serial.println("[BLE] Connected and subscribed.");
    return true;
}

bool ble_is_connected(void) {
    return s_connected && s_client && s_client->isConnected();
}

bool ble_write(const uint8_t *data, size_t len) {
    if (!ble_is_connected() || !s_tx) return false;
    uint16_t mtu = s_client->getMTU();
    if (mtu < 23) mtu = 23;   // never underflow if MTU not yet negotiated
    mtu -= 3;
    size_t nchunks = (len + mtu - 1) / mtu;
    Serial.printf("[BLE] TX len=%u mtu_payload=%u -> %u chunk(s)%s\n",
                  (unsigned)len, (unsigned)mtu, (unsigned)nchunks,
                  nchunks > 1 ? " (FRAGMENTED!)" : "");
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > mtu) ? mtu : (len - offset);
        // Write WITH response: the inverter's TX characteristic expects
        // acknowledged writes (the hiflow-ble reference uses response=True).
        // Unacknowledged writes are silently dropped and the DTU never replies.
        if (!s_tx->writeValue(data + offset, chunk, true)) {
            Serial.println("[BLE] Write failed mid-chunk.");
            return false;
        }
        offset += chunk;
    }
    return true;
}

void ble_disconnect(void) {
    if (s_client) {
        s_client->disconnect();
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
    }
    s_connected = false;
    s_tx = nullptr;
    s_rx = nullptr;
}

void ble_set_rx_callback(BleRxCallback cb) {
    s_rx_cb.store(cb);
}
