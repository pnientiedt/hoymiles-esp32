#include "ble_client.h"
#include "config.h"
#include <NimBLEDevice.h>
#include <Arduino.h>

#define SVC_UUID  "0000e0ff-3c17-d293-8e48-14fe2e4da212"
#define TX_UUID   "0000ffe1-0000-1000-8000-00805f9b34fb"
#define RX_UUID   "0000ffe2-0000-1000-8000-00805f9b34fb"

static NimBLEClient *s_client = nullptr;
static NimBLERemoteCharacteristic *s_tx = nullptr;
static NimBLERemoteCharacteristic *s_rx = nullptr;
static BleRxCallback s_rx_cb = nullptr;
static bool s_connected = false;

static void notify_cb(NimBLERemoteCharacteristic *chr,
                      uint8_t *data, size_t len, bool is_notify) {
    if (s_rx_cb && is_notify) s_rx_cb(data, len);
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

bool ble_connect(const char *device_name, BleRxCallback rx_cb) {
    s_rx_cb = rx_cb;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    NimBLEScanResults results = scan->start(10);

    NimBLEAddress target_addr;
    bool found = false;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        if (dev.getName() == device_name) {
            target_addr = dev.getAddress();
            found = true;
            break;
        }
    }
    if (!found) {
        Serial.printf("[BLE] Device '%s' not found.\n", device_name);
        return false;
    }

    s_client = NimBLEDevice::createClient();
    s_client->setClientCallbacks(&s_client_cbs, false);
    s_client->setConnectionParams(12, 12, 0, 51);
    if (!s_client->connect(target_addr)) {
        Serial.println("[BLE] Connect failed.");
        return false;
    }

    auto *svc = s_client->getService(SVC_UUID);
    if (!svc) {
        Serial.println("[BLE] Service not found.");
        s_client->disconnect();
        return false;
    }

    s_tx = svc->getCharacteristic(TX_UUID);
    s_rx = svc->getCharacteristic(RX_UUID);
    if (!s_tx || !s_rx) {
        Serial.println("[BLE] Characteristic not found.");
        s_client->disconnect();
        return false;
    }

    if (!s_rx->subscribe(true, notify_cb)) {
        Serial.println("[BLE] Subscribe failed.");
        s_client->disconnect();
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
    uint16_t mtu = s_client->getMTU() - 3;
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > mtu) ? mtu : (len - offset);
        if (!s_tx->writeValue(data + offset, chunk, false)) return false;
        offset += chunk;
    }
    return true;
}

void ble_disconnect(void) {
    if (s_client) {
        s_client->disconnect();
    }
    s_connected = false;
}

void ble_set_rx_callback(BleRxCallback cb) {
    s_rx_cb = cb;
}
