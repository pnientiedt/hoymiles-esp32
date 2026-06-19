# Daily `energy_today` zero-reset — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish a retained `energy_today = 0` at the local-day rollover and on startup when the retained value is from a previous day, so the bridge stops reporting yesterday's energy all night.

**Architecture:** A single persisted local-day key (`eday` in NVS) drives both the midnight rollover and the post-reboot stale-value case via one check run each `loop()` iteration. The pure date logic lives in a new Arduino-free module (`energy_reset`) that is unit-tested on the host; `main` wires NVS, MQTT, and timezone-aware `localtime_r` around it. Per-panel ports discovered during polling are cached in NVS so `pv/<n>/energy_today` can be zeroed while the inverter is offline.

**Tech Stack:** C++17, PlatformIO, Arduino-ESP32, PubSubClient (MQTT), ESP32 NVS via `Preferences`, Unity (host tests).

## Global Constraints

- **Public repository** — never commit secrets (WiFi/MQTT creds, broker host/IPs, BLE PIN, inverter serial, `encRand`). Use placeholders in tests/examples. `TIMEZONE` is non-secret and belongs in `src/config.h`.
- **Native (host) tests cover pure-logic modules only.** Anything pulling in NimBLE/nanopb/Arduino is verified by building for `esp32` and on-device observation, not host tests.
- **Native build is macOS/Homebrew-specific** (`[env:native]` hardcodes mbedTLS paths). Tasks here add one pure module with no mbedTLS dependency, so the existing paths are untouched.
- **`time_t` epoch is always UTC** regardless of `TZ`; only `localtime_r`/`gmtime_r` differ. Do not change `handshake.cpp`'s `gmtime_r` time-sync.
- **Retained-publish float format is `"%.3f"`** (see `poller.cpp` `publish_float`); the zero value published must be the string `"0.000"` to match.
- **NVS namespace is `"hoymiles"`** (shared with `sn`/`encRand`).
- **Commands:** native tests `python3 -m platformio test -e native -f <suite>`; firmware build `python3 -m platformio run -e esp32`. `pio` is not on PATH — always invoke via `python3 -m platformio`.

---

### Task 1: Pure `energy_reset` module (host-tested)

The only host-testable piece: the local-day equality key and the rollover decision. Built TDD with Unity under `[env:native]`.

**Files:**
- Create: `src/energy_reset.h`
- Create: `src/energy_reset.cpp`
- Create: `test/test_energy_day/test_energy_day.cpp`
- Modify: `platformio.ini:20` (`[env:native]` `build_src_filter`)

**Interfaces:**
- Consumes: nothing (leaf module; `<time.h>` `struct tm`).
- Produces:
  - `uint32_t local_day_key(const struct tm *lt)` — collision-free equality key for a local calendar day; returns `0` when `lt == NULL`, otherwise a non-zero value distinct per `(tm_year, tm_yday)`.
  - `bool energy_day_changed(uint32_t stored, uint32_t today)` — `true` iff both keys are non-zero and differ.

- [ ] **Step 1: Create the header**

Create `src/energy_reset.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Collision-free equality key for a local calendar day, derived from local
// broken-down time. Not monotonic — only guaranteed distinct for distinct days.
// Returns 0 (the "unknown" sentinel) when lt is NULL.
uint32_t local_day_key(const struct tm *lt);

// True when a daily reset is due: both keys known (non-zero) and different.
bool energy_day_changed(uint32_t stored, uint32_t today);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Add the module to the native build filter**

In `platformio.ini`, change the `[env:native]` `build_src_filter` line (line 20) from:

```ini
build_src_filter = -<*> +<crypto.cpp> +<frame.cpp>
```

to:

```ini
build_src_filter = -<*> +<crypto.cpp> +<frame.cpp> +<energy_reset.cpp>
```

- [ ] **Step 3: Create a stub implementation (so the test compiles and fails red)**

Create `src/energy_reset.cpp` with deliberately wrong bodies:

```c
#include "energy_reset.h"

uint32_t local_day_key(const struct tm *lt) {
    (void)lt;
    return 0;  // stub — replaced in Step 6
}

bool energy_day_changed(uint32_t stored, uint32_t today) {
    (void)stored;
    (void)today;
    return false;  // stub — replaced in Step 6
}
```

- [ ] **Step 4: Write the failing test**

Create `test/test_energy_day/test_energy_day.cpp`:

```cpp
#include <unity.h>
#include <string.h>
#include <time.h>
#include "../../src/energy_reset.h"

// Helper: build a local broken-down time for a given year (full, e.g. 2025)
// and day-of-year (0-based, as tm_yday).
static struct tm make_tm(int year, int yday) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_yday = yday;
    return t;
}

void test_same_day_no_reset(void) {
    struct tm a = make_tm(2026, 100);
    struct tm b = make_tm(2026, 100);
    TEST_ASSERT_EQUAL_UINT32(local_day_key(&a), local_day_key(&b));
    TEST_ASSERT_FALSE(energy_day_changed(local_day_key(&a), local_day_key(&b)));
}

void test_next_day_resets(void) {
    struct tm a = make_tm(2026, 100);
    struct tm b = make_tm(2026, 101);
    TEST_ASSERT_TRUE(energy_day_changed(local_day_key(&a), local_day_key(&b)));
}

void test_year_boundary_resets(void) {
    struct tm dec31 = make_tm(2025, 364);  // Dec 31 (non-leap yday 364)
    struct tm jan01 = make_tm(2026, 0);     // Jan 1
    TEST_ASSERT_TRUE(energy_day_changed(local_day_key(&dec31), local_day_key(&jan01)));
}

void test_same_yday_different_year_distinct(void) {
    // Guards against a naive yday-only key colliding across years.
    struct tm a = make_tm(2025, 50);
    struct tm b = make_tm(2026, 50);
    TEST_ASSERT_NOT_EQUAL(local_day_key(&a), local_day_key(&b));
    TEST_ASSERT_TRUE(energy_day_changed(local_day_key(&a), local_day_key(&b)));
}

void test_unknown_sentinel_never_resets(void) {
    struct tm a = make_tm(2026, 100);
    uint32_t key = local_day_key(&a);
    TEST_ASSERT_EQUAL_UINT32(0, local_day_key(NULL));
    TEST_ASSERT_FALSE(energy_day_changed(0, key));
    TEST_ASSERT_FALSE(energy_day_changed(key, 0));
    TEST_ASSERT_FALSE(energy_day_changed(0, 0));
}

void test_valid_key_is_nonzero(void) {
    struct tm a = make_tm(2026, 0);
    TEST_ASSERT_NOT_EQUAL(0, local_day_key(&a));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_same_day_no_reset);
    RUN_TEST(test_next_day_resets);
    RUN_TEST(test_year_boundary_resets);
    RUN_TEST(test_same_yday_different_year_distinct);
    RUN_TEST(test_unknown_sentinel_never_resets);
    RUN_TEST(test_valid_key_is_nonzero);
    return UNITY_END();
}
```

- [ ] **Step 5: Run the test to verify it fails**

Run: `python3 -m platformio test -e native -f test_energy_day`
Expected: FAIL — e.g. `test_next_day_resets` and `test_valid_key_is_nonzero` fail because the stub returns `0`/`false`.

- [ ] **Step 6: Implement the real module**

Replace the body of `src/energy_reset.cpp`:

```c
#include "energy_reset.h"

uint32_t local_day_key(const struct tm *lt) {
    if (lt == NULL) return 0;
    // (year * 366) + yday is distinct per (year, day-of-year): tm_yday is 0..365,
    // so the 366 stride prevents two different years from colliding. +1 keeps a
    // valid day from ever equalling the 0 "unknown" sentinel.
    uint32_t year = (uint32_t)(lt->tm_year + 1900);
    uint32_t yday = (uint32_t)lt->tm_yday;   // 0..365
    return year * 366u + yday + 1u;
}

bool energy_day_changed(uint32_t stored, uint32_t today) {
    if (stored == 0 || today == 0) return false;
    return stored != today;
}
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `python3 -m platformio test -e native -f test_energy_day`
Expected: PASS — 6 tests, 0 failures.

- [ ] **Step 8: Confirm the existing suites still pass**

Run: `python3 -m platformio test -e native`
Expected: `test_crypto`, `test_frame`, and `test_energy_day` all PASS.

- [ ] **Step 9: Commit**

```bash
git add src/energy_reset.h src/energy_reset.cpp test/test_energy_day/test_energy_day.cpp platformio.ini
git commit -m "feat(energy_reset): pure local-day key + rollover decision (host-tested)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Timezone configuration

Make the day boundary local instead of UTC, and add the per-panel cap constant.

**Files:**
- Modify: `src/config.h` (append after line 45)
- Modify: `src/main.cpp:158` (`configTime` → `configTzTime`)

**Interfaces:**
- Consumes: nothing.
- Produces: macros `TIMEZONE` and `MAX_PV_PORTS` (used by Task 4); a local-time-aware system clock.

- [ ] **Step 1: Add config macros**

Append to `src/config.h` (after the `RESPONSE_TIMEOUT_MS` line):

```c

// POSIX TZ string defining the local day boundary for the energy_today reset.
// The clock's epoch stays UTC; only local-time conversion uses this. Example
// below is Central Europe with DST — set it to your own zone before flashing.
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"

// Max PV ports cached (NVS + RAM) for the nightly energy_today zero-reset.
#define MAX_PV_PORTS 8
```

- [ ] **Step 2: Switch NTP setup to timezone-aware**

In `src/main.cpp`, inside `wifi_connect()`, change line 158 from:

```c
    configTime(0, 0, "pool.ntp.org");
```

to:

```c
    // configTzTime applies TZ + tzset + starts SNTP. Epoch remains UTC, so
    // handshake.cpp's gmtime_r time-sync is unaffected; only localtime_r (used by
    // the energy_today reset) now yields local time.
    configTzTime(TIMEZONE, "pool.ntp.org");
```

- [ ] **Step 3: Build firmware to verify it compiles**

Run: `python3 -m platformio run -e esp32`
Expected: `SUCCESS` (compiles and links).

- [ ] **Step 4: Commit**

```bash
git add src/config.h src/main.cpp
git commit -m "config: add TIMEZONE/MAX_PV_PORTS, use configTzTime for local day boundary

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Poller reports discovered PV ports

Add a nullable out-parameter so the caller learns which `pv/<n>` ports exist, for overnight zeroing. Wire the existing call site to pass `nullptr` (behavior unchanged) so the firmware keeps building; Task 4 supplies real buffers.

**Files:**
- Modify: `src/poller.h:11-12` (signature)
- Modify: `src/poller.cpp:197-198` (definition) and before its `return true;` (line ~299)
- Modify: `src/main.cpp:280` (call site)

**Interfaces:**
- Consumes: nothing new.
- Produces: updated signature
  `bool poller_poll(PubSubClient &mqtt, const char *base_topic, uint16_t *tid, const uint8_t enc_rand[16], uint8_t *out_ports, uint8_t *out_port_count);`
  On a successful poll, if both `out_ports` and `out_port_count` are non-NULL, `out_ports[0..*out_port_count)` holds the discovered `port_number`s (at most 4 — the merged `pv_data` cap). Untouched on failure / when NULL.

- [ ] **Step 1: Update the header**

In `src/poller.h`, replace the declaration (lines 11-12):

```c
// base_topic: runtime MQTT prefix ending in '/', e.g. "hoymiles/AABBCCDDEE12/".
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16]);
```

with:

```c
// base_topic: runtime MQTT prefix ending in '/', e.g. "hoymiles/AABBCCDDEE12/".
// out_ports/out_port_count (both nullable): on a successful poll, receive the
// discovered PV port numbers (at most 4) so the caller can zero per-panel
// energy_today while the inverter is offline. Untouched on failure or when NULL.
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16],
                 uint8_t *out_ports, uint8_t *out_port_count);
```

- [ ] **Step 2: Update the definition signature**

In `src/poller.cpp`, replace lines 197-198:

```c
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16]) {
```

with:

```c
bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16],
                 uint8_t *out_ports, uint8_t *out_port_count) {
```

- [ ] **Step 3: Fill the out-params before the final `return true;`**

In `src/poller.cpp`, immediately before the final `return true;` of `poller_poll` (currently line 299, just after the `Serial.printf("[PL] Published. ...")` call), insert:

```c
    // Report discovered PV ports so the caller can zero per-panel energy_today
    // while the inverter is offline (nightly reset lives in main).
    if (out_ports && out_port_count) {
        uint8_t n = 0;
        for (pb_size_t i = 0; i < combined.pv_data_count && n < 4; i++) {
            out_ports[n++] = (uint8_t)combined.pv_data[i].port_number;
        }
        *out_port_count = n;
    }
```

- [ ] **Step 4: Keep the call site compiling (pass NULL for now)**

In `src/main.cpp`, change the call on line 280 from:

```c
    if (!poller_poll(s_mqtt, s_base_topic, &s_tid, s_enc_rand)) {
```

to:

```c
    if (!poller_poll(s_mqtt, s_base_topic, &s_tid, s_enc_rand, nullptr, nullptr)) {
```

- [ ] **Step 5: Build firmware to verify it compiles**

Run: `python3 -m platformio run -e esp32`
Expected: `SUCCESS`.

- [ ] **Step 6: Commit**

```bash
git add src/poller.h src/poller.cpp src/main.cpp
git commit -m "feat(poller): report discovered PV ports via nullable out-param

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Wire the daily reset into `main`

Persist `eday`/`pvports` in NVS, zero `energy_today` (top-level + cached panels) on day change, and advance the day on a successful poll. Verified by building for `esp32` and on-device observation (no host coverage of `main`).

**Files:**
- Modify: `src/main.cpp` — add include, NVS keys, state, load/save helpers, `maybe_reset_energy_today()`, the `loop()` hook, and the poll-success capture.

**Interfaces:**
- Consumes: `local_day_key`, `energy_day_changed` (Task 1); `TIMEZONE`, `MAX_PV_PORTS` (Task 2); `poller_poll(..., out_ports, out_port_count)` (Task 3).
- Produces: nothing for later tasks (terminal task).

- [ ] **Step 1: Add the module include**

In `src/main.cpp`, add after `#include "poller.h"` (line 11):

```c
#include "energy_reset.h"
```

- [ ] **Step 2: Add NVS keys**

In `src/main.cpp`, after `#define NVS_KEY_SN  "sn"` (line 31), add:

```c
#define NVS_KEY_EDAY  "eday"      // local-day key of last energy publish (uint32)
#define NVS_KEY_PORTS "pvports"   // discovered PV port numbers (bytes)
```

- [ ] **Step 3: Add module state**

In `src/main.cpp`, after `static char s_base_topic[64] = {0};` (line 43), add:

```c
static uint32_t s_energy_day = 0;            // 0 = unknown (no publish yet / unsynced)
static uint8_t  s_pv_ports[MAX_PV_PORTS] = {0};
static uint8_t  s_pv_port_count = 0;
```

- [ ] **Step 4: Add load/save helpers**

In `src/main.cpp`, after `save_sn_to_nvs()` (after line 70), add:

```c
// ---- NVS energy-reset state --------------------------------------------------

static void load_energy_state_from_nvs(void) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    s_energy_day = prefs.getUInt(NVS_KEY_EDAY, 0);
    size_t n = prefs.getBytes(NVS_KEY_PORTS, s_pv_ports, sizeof(s_pv_ports));
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
```

- [ ] **Step 5: Add `maybe_reset_energy_today()`**

In `src/main.cpp`, add just before `void setup(void)` (line 221):

```c
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
```

- [ ] **Step 6: Load energy state at boot**

In `src/main.cpp` `setup()`, after `load_sn_from_nvs();` (line 231), add:

```c
    load_energy_state_from_nvs();
```

- [ ] **Step 7: Hook the reset into the loop**

In `src/main.cpp` `loop()`, after the `mqtt_connect()` guard (line 265, the line `if (!mqtt_connect()) { idle_wait(MQTT_RETRY_MS); return; }`), add:

```c

    // 3a. Zero energy_today at the local-day rollover / on a stale post-reboot
    //     value, independently of whether the inverter is reachable.
    maybe_reset_energy_today();
```

- [ ] **Step 8: Capture ports + advance the day on a successful poll**

In `src/main.cpp` `loop()`, replace the poll block (lines 280-297) — from `if (!poller_poll(...))` through the closing `}` of its `else` — with:

```c
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
```

- [ ] **Step 9: Build firmware to verify it compiles**

Run: `python3 -m platformio run -e esp32`
Expected: `SUCCESS`.

- [ ] **Step 10: Confirm host tests still pass (no regression)**

Run: `python3 -m platformio test -e native`
Expected: `test_crypto`, `test_frame`, `test_energy_day` all PASS.

- [ ] **Step 11: Commit**

```bash
git add src/main.cpp
git commit -m "feat(main): zero energy_today at local-day rollover and on stale reboot

Persists a local-day key (eday) and discovered PV ports (pvports) in NVS;
publishes retained energy_today=0 (top-level + per-panel) when the day
advances past the last energy publish. Covers both midnight rollover and a
stale retained value after reboot.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 12: On-device verification (manual; requires hardware + inverter)**

Not host-testable. After flashing (`python3 -m platformio run -e esp32 -t upload` then `python3 -m platformio device monitor -b 115200`), confirm:
1. **TIMEZONE set** to your zone in `src/config.h` before flashing.
2. **Rollover:** after local midnight, the serial logs `Day rollover (... -> ...): published energy_today=0`, and the broker's retained `<prefix>/<serial>/energy_today` reads `0.000` (plus each `pv/<n>/energy_today`).
3. **Stale reboot:** with a retained non-zero `energy_today` from a previous day, reboot the ESP32 before the inverter wakes; within the first loop after NTP sync it should publish `0.000`.
4. **No false zero:** rebooting mid-day (same day as last publish) must NOT zero the value.
5. **Morning recovery:** when the inverter wakes, the real (small) daily value overwrites the `0.000`, and no further rollover log appears that day.

---

## Self-Review

**Spec coverage:**
- Midnight rollover → Task 4 Steps 5,7. ✓
- Startup stale value → Task 4 Steps 5,6,7 (same check, `eday` loaded at boot). ✓
- Local day boundary / TIMEZONE / `configTzTime` → Task 2. ✓
- `handshake.cpp` `gmtime_r` untouched → enforced by Global Constraints; no task edits handshake. ✓
- Per-panel zeroing via cached ports → Task 3 (report) + Task 4 (cache/persist/use). ✓
- `eday` / `pvports` NVS keys, namespace `hoymiles` → Task 4 Steps 2,4. ✓
- Pure module + host tests (incl. year boundary, unknown sentinel) → Task 1. ✓
- `energy_total` / `last_seen` not reset → only `energy_today` topics are published in `maybe_reset_energy_today()`. ✓
- Edge cases (no NTP, repeated reboots, producing past midnight, first boot, DST) → Task 4 `maybe_reset_energy_today()` guards + Task 1 key design; called out in Step 12 verification. ✓
- Float format `"0.000"` matches `publish_float` → Task 4 Step 5. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"; every code step shows full code. ✓

**Type consistency:** `local_day_key(const struct tm *)`, `energy_day_changed(uint32_t,uint32_t)`, and `poller_poll(..., uint8_t *out_ports, uint8_t *out_port_count)` are used identically across Tasks 1, 3, and 4. NVS helpers `save_energy_day_to_nvs`/`save_pv_ports_to_nvs`/`load_energy_state_from_nvs` and state `s_energy_day`/`s_pv_ports`/`s_pv_port_count` named consistently. ✓
