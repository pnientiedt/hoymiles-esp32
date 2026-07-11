# WiFi/MQTT Recovery Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bound the DTU's WiFi/MQTT silent state to ~5 minutes by adding a host-testable connection watchdog that forces re-association then reboot, plus WiFi hardening and reconnect observability.

**Architecture:** A new pure C module `net_watchdog` decides — from timestamps only — whether to do nothing, force a WiFi re-association, or reboot. `main.cpp` tracks a "last healthy" timestamp (MQTT connected = healthy, the only trustworthy signal) and executes the module's decision each loop. WiFi power-save is disabled and TX power maxed in `setup()`.

**Tech Stack:** PlatformIO + Arduino (ESP32), Unity (native host tests), PubSubClient, ESP32 WiFi.

## Global Constraints

- **Public repo — never commit secrets.** No SSID/IP/broker/serial/PIN in code, tests, or commit messages. Use placeholders.
- **Native tests compile only whitelisted `.cpp`/`.c`** via `[env:native]` `build_src_filter`. A new host-tested module MUST be added there.
- **`pio` is invoked as `python3 -m platformio`** (not on PATH).
- **The 30 s task watchdog** must be fed during long waits; don't add blocking calls > ~4 s off the existing `idle_wait` path.
- **Pure modules take no Arduino/WiFi headers** — that's what makes them host-testable (mirror `energy_reset`).

---

### Task 1: `net_watchdog` pure decision module + host tests

**Files:**
- Create: `src/net_watchdog.h`
- Create: `src/net_watchdog.c`
- Create: `test/test_net_watchdog/test_net_watchdog.cpp`
- Modify: `platformio.ini:20` (add `+<net_watchdog.c>` to native filter)

**Interfaces:**
- Consumes: nothing (leaf module).
- Produces:
  - `typedef enum { NET_ACTION_NONE, NET_ACTION_REASSOCIATE, NET_ACTION_RESTART } net_action_t;`
  - `typedef struct { uint32_t reassoc_after_ms; uint32_t restart_after_ms; uint32_t reassoc_interval_ms; } net_watchdog_cfg_t;`
  - `net_action_t net_watchdog_decide(uint32_t now_ms, uint32_t last_healthy_ms, uint32_t last_reassoc_ms, const net_watchdog_cfg_t *cfg);`

- [ ] **Step 1: Write the header**

Create `src/net_watchdog.h`:

```c
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the connection watchdog wants the caller to do this tick.
typedef enum {
    NET_ACTION_NONE,        // link healthy, or still within grace window
    NET_ACTION_REASSOCIATE, // force a WiFi teardown + reconnect
    NET_ACTION_RESTART      // give up and ESP.restart()
} net_action_t;

typedef struct {
    uint32_t reassoc_after_ms;     // unhealthy this long -> reassociate
    uint32_t restart_after_ms;     // unhealthy this long -> restart (must be > reassoc)
    uint32_t reassoc_interval_ms;  // min spacing between reassociate attempts
} net_watchdog_cfg_t;

// Pure decision. "Healthy" is defined by the caller (MQTT connected) via
// last_healthy_ms. All args are millis()-style; subtraction is wraparound-safe
// for elapsed spans under ~49.7 days. RESTART takes precedence over REASSOCIATE.
net_action_t net_watchdog_decide(uint32_t now_ms,
                                 uint32_t last_healthy_ms,
                                 uint32_t last_reassoc_ms,
                                 const net_watchdog_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the failing tests**

Create `test/test_net_watchdog/test_net_watchdog.cpp`:

```c
#include <unity.h>
#include <stdint.h>
#include "../../src/net_watchdog.h"

// Matches the firmware config: 120 s reassoc, 300 s restart, 30 s throttle.
static const net_watchdog_cfg_t CFG = { 120000, 300000, 30000 };

void test_healthy_is_none(void) {
    // now == last_healthy -> elapsed 0
    TEST_ASSERT_EQUAL(NET_ACTION_NONE,
        net_watchdog_decide(1000, 1000, 0, &CFG));
}

void test_just_below_reassoc_is_none(void) {
    // elapsed 119999 < 120000
    TEST_ASSERT_EQUAL(NET_ACTION_NONE,
        net_watchdog_decide(119999, 0, 0, &CFG));
}

void test_at_reassoc_threshold_reassociates(void) {
    // elapsed 120000, last_reassoc far in the past -> throttle satisfied
    TEST_ASSERT_EQUAL(NET_ACTION_REASSOCIATE,
        net_watchdog_decide(120000, 0, 0, &CFG));
}

void test_reassoc_throttled_when_recent(void) {
    // elapsed 130000 (past reassoc threshold) but last_reassoc only 10 s ago
    uint32_t now = 130000;
    uint32_t last_reassoc = now - 10000; // since_reassoc 10000 < 30000
    TEST_ASSERT_EQUAL(NET_ACTION_NONE,
        net_watchdog_decide(now, 0, last_reassoc, &CFG));
}

void test_reassoc_allowed_after_throttle(void) {
    uint32_t now = 200000;
    uint32_t last_reassoc = now - 30000; // exactly the interval
    TEST_ASSERT_EQUAL(NET_ACTION_REASSOCIATE,
        net_watchdog_decide(now, 0, last_reassoc, &CFG));
}

void test_at_restart_threshold_restarts(void) {
    // elapsed 300000 -> restart even though reassoc would also be due
    TEST_ASSERT_EQUAL(NET_ACTION_RESTART,
        net_watchdog_decide(300000, 0, 0, &CFG));
}

void test_restart_takes_precedence_over_throttled_reassoc(void) {
    // elapsed past restart; last_reassoc recent (reassoc throttled) -> still RESTART
    uint32_t now = 310000;
    TEST_ASSERT_EQUAL(NET_ACTION_RESTART,
        net_watchdog_decide(now, 0, now - 1000, &CFG));
}

void test_wraparound_safe(void) {
    // last_healthy near UINT32 top; now has wrapped past 0. elapsed = 130000.
    uint32_t last_healthy = 4294900000u;          // ~0xFFFF0AE0
    uint32_t now = last_healthy + 130000u;        // wraps -> 62704
    TEST_ASSERT_EQUAL(NET_ACTION_REASSOCIATE,
        net_watchdog_decide(now, last_healthy, 0, &CFG));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_healthy_is_none);
    RUN_TEST(test_just_below_reassoc_is_none);
    RUN_TEST(test_at_reassoc_threshold_reassociates);
    RUN_TEST(test_reassoc_throttled_when_recent);
    RUN_TEST(test_reassoc_allowed_after_throttle);
    RUN_TEST(test_at_restart_threshold_restarts);
    RUN_TEST(test_restart_takes_precedence_over_throttled_reassoc);
    RUN_TEST(test_wraparound_safe);
    return UNITY_END();
}
```

- [ ] **Step 3: Add the module to the native build filter**

Modify `platformio.ini` line 20, from:

```ini
build_src_filter = -<*> +<crypto.cpp> +<frame.cpp> +<energy_reset.cpp>
```

to:

```ini
build_src_filter = -<*> +<crypto.cpp> +<frame.cpp> +<energy_reset.cpp> +<net_watchdog.c>
```

- [ ] **Step 4: Run tests to verify they fail**

Run: `python3 -m platformio test -e native -f test_net_watchdog`
Expected: FAIL — link/compile error, `net_watchdog_decide` undefined (no `.c` yet).

- [ ] **Step 5: Write the minimal implementation**

Create `src/net_watchdog.c`:

```c
#include "net_watchdog.h"

net_action_t net_watchdog_decide(uint32_t now_ms,
                                 uint32_t last_healthy_ms,
                                 uint32_t last_reassoc_ms,
                                 const net_watchdog_cfg_t *cfg) {
    uint32_t elapsed = now_ms - last_healthy_ms;   // wraparound-safe unsigned math
    if (elapsed >= cfg->restart_after_ms) {
        return NET_ACTION_RESTART;
    }
    if (elapsed >= cfg->reassoc_after_ms) {
        uint32_t since_reassoc = now_ms - last_reassoc_ms;
        if (since_reassoc >= cfg->reassoc_interval_ms) {
            return NET_ACTION_REASSOCIATE;
        }
    }
    return NET_ACTION_NONE;
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `python3 -m platformio test -e native -f test_net_watchdog`
Expected: PASS — 8 tests, 0 failures.

- [ ] **Step 7: Confirm no regression in the other native suites**

Run: `python3 -m platformio test -e native`
Expected: PASS — `test_crypto`, `test_frame`, `test_energy_day`, `test_net_watchdog` all green.

- [ ] **Step 8: Commit**

```bash
git add src/net_watchdog.h src/net_watchdog.c test/test_net_watchdog/test_net_watchdog.cpp platformio.ini
git commit -m "feat(net_watchdog): pure connection-recovery decision module + host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Wire the watchdog + WiFi hardening + observability into `main.cpp`

**Files:**
- Modify: `src/config.h` (add watchdog timing constants)
- Modify: `src/main.cpp` (include, state, setup hardening, loop integration, mqtt counter, diag topics)

**Interfaces:**
- Consumes: `net_watchdog_decide`, `net_action_t`, `net_watchdog_cfg_t` from Task 1.
- Produces: three new retained MQTT topics `diag/wifi_reconnect_count`, `diag/mqtt_reconnect_count`, `diag/mqtt_disconnect_s`. No new firmware-internal API.

**Verification note:** `main.cpp` is device-only (pulls WiFi/NimBLE); it is NOT in the native filter, so it's verified by an ESP32 build + a hardware serial capture, not host tests.

- [ ] **Step 1: Add watchdog timing constants to `config.h`**

In `src/config.h`, after the retry constants block (after line 45, `RESPONSE_TIMEOUT_MS`), add:

```c
// Connection watchdog (net_watchdog): MQTT is the health signal. After this long
// without a healthy MQTT link, force a WiFi re-association; after the restart
// window, reboot. Bounds any silent state to ~restart-window seconds.
#define NET_REASSOC_AFTER_MS    120000  // 2 min unhealthy -> reassociate
#define NET_RESTART_AFTER_MS    300000  // 5 min unhealthy -> ESP.restart()
#define NET_REASSOC_INTERVAL_MS  30000  // min spacing between reassociate attempts
```

- [ ] **Step 2: Include the module and declare watchdog state in `main.cpp`**

In `src/main.cpp`, add the include after line 12 (`#include "energy_reset.h"`):

```cpp
#include "net_watchdog.h"
```

Then, after the existing static state block (after line 53, the `s_ble_state` declaration), add:

```cpp
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
```

- [ ] **Step 3: Bump the MQTT reconnect counter on a successful connect**

In `mqtt_connect()`, immediately after the successful-connect log line
`Serial.println("[MQTT] Connected.");` (currently `src/main.cpp:222`), add:

```cpp
    s_mqtt_reconnect_count++;
```

- [ ] **Step 4: Add the three new diag topics**

In `publish_diag()`, after the `diag/ble_state` publish (currently the last
publish, `src/main.cpp:177-178`), add:

```cpp
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
```

- [ ] **Step 5: Add WiFi hardening + socket timeout + healthy seed to `setup()`**

In `setup()`, replace the line `s_mqtt.setServer(MQTT_HOST, MQTT_PORT);`
(currently `src/main.cpp:305`) with this block (keeps `setServer`, adds around it):

```cpp
    // WiFi hardening for a weak link: start the STA interface, then kill modem
    // power-save (a classic weak-link drop cause), max the TX power, and let the
    // driver auto-reconnect between our watchdog checks. persistent(false) keeps
    // WiFi-config NVS writes off the hot path.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    // Seed the watchdog's health clock so boot gets a full grace window.
    s_last_healthy_ms = millis();

    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    // Fail a dead-link connect() fast (4 s) instead of blocking ~15 s under the
    // 30 s task-WDT.
    s_mqtt.setSocketTimeout(4);
```

- [ ] **Step 6: Run the connection watchdog at the top of `loop()`**

In `loop()`, immediately after `esp_task_wdt_reset();` (currently
`src/main.cpp:325`) and BEFORE the `// 1. WiFi …` comment, add:

```cpp
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
            break;              // unreachable
        case NET_ACTION_NONE:
        default:
            break;
    }
```

- [ ] **Step 7: Build the firmware**

Run: `python3 -m platformio run -e esp32`
Expected: SUCCESS — compiles and links, no errors. (Confirms the WiFi macros,
`setSocketTimeout`, and the module wiring are correct.)

- [ ] **Step 8: Commit**

```bash
git add src/config.h src/main.cpp
git commit -m "feat(main): connection watchdog, WiFi hardening, reconnect diag topics

Bounds any WiFi/MQTT silent state to ~5 min: MQTT-unhealthy for 2 min forces a
WiFi reassociation, 5 min triggers ESP.restart(). Disables modem power-save,
maxes TX power, and publishes wifi/mqtt reconnect counters + mqtt_disconnect_s.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 9: Hardware verification (manual, by the user)**

This module governs the live BLE/WiFi state machine with no host coverage, so
it must be proven on the device:

```bash
python3 -m platformio run -e esp32 -t upload
ble-test/.venv/bin/python tools/capture_serial.py 180
```

Confirm in the capture / MQTT:
- Steady state: no `[watchdog]` lines, `diag/wifi_reconnect_count` stays 0.
- `diag/mqtt_reconnect_count` is >= 1 and `diag/mqtt_disconnect_s` reads ~0 while connected.
- (Optional stress) power off the AP: a `[watchdog] … reassociation` line appears
  ~2 min in, and `[watchdog] … ESP.restart()` ~5 min in (next boot `reset_reason=sw`).

---

## Self-Review

**Spec coverage:**
- WiFi hardening (power-save off, max TX) → Task 2 Step 5. ✓
- Connection watchdog reassoc→restart, MQTT-unhealthy trigger → Task 1 (logic) + Task 2 Step 6 (glue). ✓
- Host-testable timing module → Task 1. ✓
- Observability (wifi/mqtt reconnect counts, disconnect_s) → Task 2 Steps 3-4. ✓
- Fast-fail socket timeout → Task 2 Step 5. ✓
- Deferred (static IP, BSSID, MQTT backoff) → not in plan, per spec. ✓
- RAM-only counters (per approved design) → Task 2 Step 2 (plain statics, no NVS). ✓

**Placeholder scan:** No TBD/TODO; every code step shows full code. ✓

**Type consistency:** `net_watchdog_decide` signature, `net_action_t` enum values, and `net_watchdog_cfg_t` fields (`reassoc_after_ms`/`restart_after_ms`/`reassoc_interval_ms`) match across header (T1S1), tests (T1S2), impl (T1S5), and glue (T2S2/S6). Config macro names `NET_REASSOC_AFTER_MS`/`NET_RESTART_AFTER_MS`/`NET_REASSOC_INTERVAL_MS` match between config.h (T2S1) and the cfg initializer (T2S2). ✓
