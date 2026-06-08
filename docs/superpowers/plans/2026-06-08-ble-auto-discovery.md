# BLE Serial Auto-Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Auto-discover the inverter's 12-char serial from its `RMI-` BLE advertisement so one binary runs on any HMS-800 with no per-device `secrets.h` edits.

**Architecture:** The serial becomes runtime data instead of a compile-time macro. `ble_connect` matches the `RMI-` name prefix and reports the discovered serial; that serial is threaded into the handshake (for crypto) and used to build the MQTT topic base at runtime. Because the MQTT Last-Will/status topic depends on the serial, the `loop()` state machine is reordered to discover BLE before connecting MQTT. Tasks are ordered so the firmware compiles after every commit.

**Tech Stack:** PlatformIO, Arduino (ESP32), NimBLE-Arduino, PubSubClient.

**Verification convention:** `crypto`/`frame` are the only host-testable modules; `ble_client`/`handshake`/`poller`/`main` are verified by ESP32 build + on-device run (see `CLAUDE.md`). Every task gate is therefore:
- `python3 -m platformio run -e esp32` → `SUCCESS`
- `python3 -m platformio test -e native` → 15/15 still pass (regression; these files are excluded from the native build, so this must never change)

The reference spec is `docs/superpowers/specs/2026-06-08-ble-auto-discovery-design.md`.

---

## File Map

| File | Change |
|------|--------|
| `src/config.h` | Add `BLE_NAME_PREFIX`, `INVERTER_SN_FILTER`, `MQTT_TOPIC_PREFIX`; remove `MQTT_BASE_TOPIC` (Task 5) |
| `src/ble_client.h` / `.cpp` | Prefix-match scan + serial extraction; new `ble_connect` signature |
| `src/handshake.h` / `.cpp` | `handshake_run(sn, …)`; `do_v0_pairing`/`do_commcmd` take `sn` |
| `src/poller.h` / `.cpp` | `poller_poll(…, base_topic, …)`; runtime topic prefix |
| `src/main.cpp` | Discover SN, build runtime topics, reorder BLE-before-MQTT |
| `src/secrets.h` / `secrets.example.h` | Remove `BLE_DEVICE_NAME`, `INVERTER_SN` (Task 5) |
| `CLAUDE.md`, `README.md` | Document auto-discovery (Task 5) |

---

## Task 1: BLE prefix-match discovery

Change `ble_connect` to scan for the `RMI-` prefix and report the discovered
serial. Update the single call site in `main.cpp` so the tree keeps compiling
(the handshake still uses the old `INVERTER_SN` macro until Task 2).

**Files:**
- Modify: `src/config.h`
- Modify: `src/ble_client.h`
- Modify: `src/ble_client.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Add discovery config to `src/config.h`**

Insert after the `MQTT_CLIENT_ID` line, before `MQTT_BASE_TOPIC`:

```c
// Inverter BLE advertisement prefix; the 12-char serial tail after this is
// auto-discovered during the scan.
#define BLE_NAME_PREFIX     "RMI-"

// Optional: if several inverters are in BLE range, set this to the exact
// 12-char serial tail to pin one. Empty = connect to the first RMI- device.
#define INVERTER_SN_FILTER  ""
```

- [ ] **Step 2: Update `ble_connect` signature in `src/ble_client.h`**

Replace:

```c
bool ble_connect(const char *device_name, BleRxCallback rx_cb);
```

with:

```c
// Scans for a device whose advertised name starts with name_prefix. If
// sn_filter is non-NULL and non-empty, additionally requires the serial tail
// (the name after the prefix) to equal sn_filter exactly. On success, copies
// the matched serial tail into sn_out (sn_out_len must be >= 13), connects,
// negotiates MTU, and subscribes to RX notifications.
bool ble_connect(const char *name_prefix, const char *sn_filter,
                 char *sn_out, size_t sn_out_len, BleRxCallback rx_cb);
```

- [ ] **Step 3: Add `<string.h>` include to `src/ble_client.cpp`**

Under the existing `#include <atomic>`:

```c
#include <string.h>
```

- [ ] **Step 4: Rewrite the scan/match block in `src/ble_client.cpp`**

Replace the whole function signature line and the scan-result loop (from
`bool ble_connect(const char *device_name, BleRxCallback rx_cb) {` through the
`if (!found) { … return false; }` block) with:

```c
bool ble_connect(const char *name_prefix, const char *sn_filter,
                 char *sn_out, size_t sn_out_len, BleRxCallback rx_cb) {
    s_rx_cb.store(rx_cb);

    // Clean up any pre-existing client before creating a new one
    if (s_client) {
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
    }

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    NimBLEScanResults results = scan->start(10);

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
```

Leave everything after this point (the `createClient` → `connect` → `getService`
→ `getCharacteristic` → `subscribe` sequence) unchanged.

- [ ] **Step 5: Update the call site in `src/main.cpp`**

Add a serial buffer next to the other `static` state (after `s_poll_failures`):

```c
static char     s_sn[16] = {0};
```

Replace `ble_connect_and_handshake`'s first two lines:

```c
static bool ble_connect_and_handshake(void) {
    if (!ble_connect(BLE_DEVICE_NAME, nullptr)) return false;
    if (!handshake_run(&s_tid, s_enc_rand)) {
```

with (note `handshake_run` is still the old signature here — changed in Task 2):

```c
static bool ble_connect_and_handshake(void) {
    if (!ble_connect(BLE_NAME_PREFIX, INVERTER_SN_FILTER, s_sn, sizeof(s_sn), nullptr)) return false;
    Serial.printf("[main] Inverter SN: %s\n", s_sn);
    if (!handshake_run(&s_tid, s_enc_rand)) {
```

- [ ] **Step 6: Build and regression-test**

```bash
python3 -m platformio run -e esp32
python3 -m platformio test -e native
```

Expected: esp32 `SUCCESS`; native 15/15 pass.

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/ble_client.h src/ble_client.cpp src/main.cpp
git commit -m "feat: BLE scan matches RMI- prefix and reports discovered serial"
```

---

## Task 2: Thread the serial into the handshake

Pass the discovered serial into the handshake instead of the `INVERTER_SN`
macro.

**Files:**
- Modify: `src/handshake.h`
- Modify: `src/handshake.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Update `handshake_run` signature in `src/handshake.h`**

Replace:

```c
bool handshake_run(uint16_t *tid, uint8_t enc_rand_out[16]);
```

with:

```c
// sn: the inverter's 12-char serial (discovered over BLE), used for V0 key/IV
// derivation and the CommCmd dtu_sn.
bool handshake_run(const char *sn, uint16_t *tid, uint8_t enc_rand_out[16]);
```

- [ ] **Step 2: Take `sn` in `do_v0_pairing` (`src/handshake.cpp`)**

Change the signature:

```c
static bool do_v0_pairing(const char *sn, uint16_t *tid, uint8_t enc_rand_out[16]) {
```

Replace the two request-side derivations:

```c
    v0_derive_key(INVERTER_SN, key);
    v0_derive_iv(CMD_APP_INFO_REQ, *tid, INVERTER_SN, iv);
```

with:

```c
    v0_derive_key(sn, key);
    v0_derive_iv(CMD_APP_INFO_REQ, *tid, sn, iv);
```

And the two response-side derivations:

```c
    v0_derive_key(INVERTER_SN, rkey);
    v0_derive_iv(rcmd, rtid, INVERTER_SN, riv);
```

with:

```c
    v0_derive_key(sn, rkey);
    v0_derive_iv(rcmd, rtid, sn, riv);
```

- [ ] **Step 3: Take `sn` in `do_commcmd` (`src/handshake.cpp`)**

Change the signature:

```c
static bool do_commcmd(const char *sn, uint16_t *tid, const uint8_t enc_rand[16], int32_t action) {
```

Replace:

```c
    strncpy(cmd_msg.dtu_sn, INVERTER_SN, sizeof(cmd_msg.dtu_sn) - 1);
```

with:

```c
    strncpy(cmd_msg.dtu_sn, sn, sizeof(cmd_msg.dtu_sn) - 1);
```

- [ ] **Step 4: Pass `sn` through `handshake_run` (`src/handshake.cpp`)**

Change the signature:

```c
bool handshake_run(const char *sn, uint16_t *tid, uint8_t enc_rand_out[16]) {
```

Update the three internal calls:

```c
        if (!do_v0_pairing(sn, tid, enc_rand_out)) {
```
```c
    if (!do_commcmd(sn, tid, enc_rand_out, COMMCMD_LOGIN)) {
```
```c
    if (!do_commcmd(sn, tid, enc_rand_out, COMMCMD_TIME_SYNC)) {
```

- [ ] **Step 5: Update the call site in `src/main.cpp`**

Replace:

```c
    if (!handshake_run(&s_tid, s_enc_rand)) {
```

with:

```c
    if (!handshake_run(s_sn, &s_tid, s_enc_rand)) {
```

- [ ] **Step 6: Build and regression-test**

```bash
python3 -m platformio run -e esp32
python3 -m platformio test -e native
```

Expected: esp32 `SUCCESS`; native 15/15 pass.

- [ ] **Step 7: Commit**

```bash
git add src/handshake.h src/handshake.cpp src/main.cpp
git commit -m "feat: thread discovered serial into BLE handshake"
```

---

## Task 3: Runtime MQTT base topic in the poller

Build the MQTT topic base (`hoymiles/<sn>/`) at runtime and pass it into the
poller, replacing the compile-time `MQTT_BASE_TOPIC` usage there.

**Files:**
- Modify: `src/config.h`
- Modify: `src/poller.h`
- Modify: `src/poller.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Add `MQTT_TOPIC_PREFIX` to `src/config.h`**

Add near the MQTT defines (leave `MQTT_BASE_TOPIC` in place for now — `main`
still uses it for the LWT until Task 4):

```c
// MQTT topics are <prefix><discovered-serial>/...  built at runtime.
#define MQTT_TOPIC_PREFIX "hoymiles/"
```

- [ ] **Step 2: Update `poller_poll` signature in `src/poller.h`**

Replace:

```c
bool poller_poll(PubSubClient &mqtt, uint16_t *tid, const uint8_t enc_rand[16]);
```

with:

```c
// base_topic: runtime MQTT prefix ending in '/', e.g. "hoymiles/AABBCCDDEE12/".
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16]);
```

- [ ] **Step 3: Use a runtime base in the publish helpers (`src/poller.cpp`)**

Add a file-static base pointer just above `publish_float` (after the RX state /
decode section):

```c
static const char *s_base_topic = "";
```

Replace the two helpers' topic construction. In `publish_float`:

```c
    snprintf(topic, sizeof(topic), "%s%s", MQTT_BASE_TOPIC, subtopic);
```

with:

```c
    snprintf(topic, sizeof(topic), "%s%s", s_base_topic, subtopic);
```

In `publish_str`:

```c
    snprintf(topic, sizeof(topic), "%s%s", MQTT_BASE_TOPIC, subtopic);
```

with:

```c
    snprintf(topic, sizeof(topic), "%s%s", s_base_topic, subtopic);
```

- [ ] **Step 4: Accept and store `base_topic` in `poller_poll` (`src/poller.cpp`)**

Change the signature:

```c
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16]) {
    s_base_topic = base_topic;
    ble_set_rx_callback(rx_handler);
```

(The first line of the original body was `ble_set_rx_callback(rx_handler);` —
add the `s_base_topic = base_topic;` assignment immediately before it.)

- [ ] **Step 5: Build the base topic and pass it in `src/main.cpp`**

Add a base-topic buffer after `s_sn`:

```c
static char     s_base_topic[64] = {0};
```

In `ble_connect_and_handshake`, build it right after the SN log line:

```c
    Serial.printf("[main] Inverter SN: %s\n", s_sn);
    snprintf(s_base_topic, sizeof(s_base_topic), "%s%s/", MQTT_TOPIC_PREFIX, s_sn);
```

Replace the poll call:

```c
    if (!poller_poll(s_mqtt, &s_tid, s_enc_rand)) {
```

with:

```c
    if (!poller_poll(s_mqtt, s_base_topic, &s_tid, s_enc_rand)) {
```

- [ ] **Step 6: Build and regression-test**

```bash
python3 -m platformio run -e esp32
python3 -m platformio test -e native
```

Expected: esp32 `SUCCESS`; native 15/15 pass.

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/poller.h src/poller.cpp src/main.cpp
git commit -m "feat: build MQTT topic base from discovered serial at runtime"
```

---

## Task 4: Reorder loop (BLE before MQTT) + runtime status/version topics

The MQTT Last-Will and status topics depend on the serial, so discover BLE
before connecting MQTT, and build those topics at runtime.

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Remove the compile-time status-topic macro (`src/main.cpp`)**

Delete this line:

```c
#define MQTT_STATUS_TOPIC MQTT_BASE_TOPIC "status"
```

- [ ] **Step 2: Add a runtime status-topic buffer (`src/main.cpp`)**

After `s_base_topic`:

```c
static char     s_status_topic[80] = {0};
```

- [ ] **Step 3: Build the status topic during discovery (`src/main.cpp`)**

In `ble_connect_and_handshake`, just after the `s_base_topic` snprintf:

```c
    snprintf(s_status_topic, sizeof(s_status_topic), "%sstatus", s_base_topic);
```

- [ ] **Step 4: Rewrite `mqtt_connect` to use runtime topics (`src/main.cpp`)**

Replace the whole `mqtt_connect` function with:

```c
static bool mqtt_connect(void) {
    if (s_mqtt.connected()) return true;
    Serial.printf("[MQTT] Connecting to %s:%d …\n", MQTT_HOST, MQTT_PORT);
    if (!s_mqtt.connect(MQTT_CLIENT_ID, nullptr, nullptr, s_status_topic, 1, true, "offline")) {
        Serial.printf("[MQTT] Failed, rc=%d\n", s_mqtt.state());
        return false;
    }
    Serial.println("[MQTT] Connected.");
    char ver_topic[96];
    snprintf(ver_topic, sizeof(ver_topic), "%sfirmware_version", s_base_topic);
    s_mqtt.publish(ver_topic, FW_VERSION, true);
    s_mqtt.publish(s_status_topic, "online", true);
    return true;
}
```

- [ ] **Step 5: Reorder `loop()` so BLE+handshake runs before MQTT (`src/main.cpp`)**

Replace the body of `loop()` from after `esp_task_wdt_reset();` down to the
`poller_poll(...)` failure block with:

```c
    if (!wifi_connect()) { delay(WIFI_RETRY_MS); return; }

    if (!ble_is_connected() || !s_enc_rand_ready) {
        if (!ble_connect_and_handshake()) {
            delay(BLE_RETRY_MS);
            return;
        }
    }

    if (!mqtt_connect()) { delay(MQTT_RETRY_MS); return; }

    if (!poller_poll(s_mqtt, s_base_topic, &s_tid, s_enc_rand)) {
```

This removes the old `s_mqtt.publish(MQTT_STATUS_TOPIC, "online", true);` line
(the "online" publish now lives in `mqtt_connect`) and swaps the MQTT-before-BLE
ordering for BLE-before-MQTT. Leave the poll-failure handling, `s_mqtt.loop()`,
and the sliced idle-wait loop below unchanged.

- [ ] **Step 6: Build and regression-test**

```bash
python3 -m platformio run -e esp32
python3 -m platformio test -e native
```

Expected: esp32 `SUCCESS`; native 15/15 pass. `MQTT_BASE_TOPIC` is now unused
anywhere in compiled code (removed in Task 5).

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "feat: discover BLE before MQTT and build status topics at runtime"
```

---

## Task 5: Remove dead config + update docs

Nothing references `MQTT_BASE_TOPIC`, `INVERTER_SN`, or `BLE_DEVICE_NAME`
anymore. Remove them and document auto-discovery.

**Files:**
- Modify: `src/config.h`
- Modify: `src/secrets.h`
- Modify: `src/secrets.example.h`
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Remove `MQTT_BASE_TOPIC` from `src/config.h`**

Delete the `MQTT_BASE_TOPIC` define and its comment.

- [ ] **Step 2: Remove discovered values from `src/secrets.example.h`**

Delete the `BLE_DEVICE_NAME` and `INVERTER_SN` defines and their comments. The
template now contains only `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_HOST`. Add a note:

```c
// The inverter's serial is auto-discovered from its "RMI-" BLE advertisement —
// no device serial needs to be configured here.
```

- [ ] **Step 3: Mirror the change in the local `src/secrets.h`**

Delete the same `BLE_DEVICE_NAME` and `INVERTER_SN` defines. (This file is
gitignored; edit it so the local build still compiles.)

- [ ] **Step 4: Build and regression-test**

```bash
python3 -m platformio run -e esp32
python3 -m platformio test -e native
```

Expected: esp32 `SUCCESS`; native 15/15 pass.

- [ ] **Step 5: Update `CLAUDE.md` configuration section**

In the "Configuration / secrets" section, change the `secrets.h` contents to
only WiFi + MQTT host, and note that the inverter serial and MQTT topic prefix
are derived at runtime from the `RMI-` advertisement (config knobs:
`BLE_NAME_PREFIX`, `MQTT_TOPIC_PREFIX`, optional `INVERTER_SN_FILTER`).

- [ ] **Step 6: Update `README.md`**

In "Configure", drop `BLE_DEVICE_NAME` / `INVERTER_SN` from the table (the
serial is auto-discovered); keep `WIFI_*` and `MQTT_HOST`. Add a short note that
the firmware finds the inverter by its `RMI-` prefix and that
`INVERTER_SN_FILTER` in `config.h` pins one device when several are in range.
In the MQTT topic table intro, note `<INVERTER_SN>` is the auto-discovered tail.

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/secrets.example.h CLAUDE.md README.md
git commit -m "refactor: drop manual serial config; document BLE auto-discovery"
```

> `src/secrets.h` is gitignored and won't be staged — that's expected.

---

## On-device verification (after Task 5)

Flash a build whose `secrets.h` has **no** serial configured and confirm:

```bash
python3 -m platformio run -e esp32 -t upload
python3 -m platformio device monitor -b 115200
```

- Serial log prints `[main] Inverter SN: <12 chars>` from the discovery.
- `mosquitto_sub -h <MQTT_HOST> -t 'hoymiles/#' -v` shows topics under the
  discovered serial, including `hoymiles/<sn>/status = online`.
