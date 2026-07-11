# WiFi/MQTT Recovery Hardening — Design

**Date:** 2026-07-11
**Status:** Approved for planning

## Problem

The ESP32 DTU stopped publishing to MQTT for ~4.5 h overnight (2026-07-11,
02:13Z → 06:34Z) and only recovered when the WiFi driver's own auto-reconnect
eventually fired. It did **not** crash (`uptime_s` monotonic across the gap) and
was **not** out of memory (`free_heap` ~122 KB after recovery). The link is
chronically marginal (RSSI −86 to −93 dBm, near the practical floor).

### Root cause (confirmed from code)

`wifi_connect()` (`src/main.cpp:183`) returns `true` as soon as
`WiFi.status() == WL_CONNECTED`. On ESP32-Arduino, `WiFi.status()` routinely
reports `WL_CONNECTED` for a **zombie association** — the link is dead but the
driver hasn't noticed, which is common at −90 dBm. In that state the loop spins:

1. `wifi_connect()` returns `true` without ever re-associating (no
   `WiFi.begin()`, no `disconnect(true)`).
2. `mqtt_connect()` tries TCP over the dead link, fails, `idle_wait(MQTT_RETRY_MS)`
   waits 10 s, loop repeats (~25 s/cycle).
3. **Nothing anywhere forces a WiFi teardown**, so recovery depends entirely on
   the driver noticing on its own.

`diag/*` (incl. `wifi_rssi`) only publishes while MQTT is connected, which is
exactly why the diagnostics flatlined for the whole outage — the firmware was
stuck in that zombie-WiFi + failing-MQTT spin. The stall is a **recovery-timing
bug**, not a crash.

## Goals

1. Bound any silent state to ~5 minutes, worst case.
2. Reduce drop frequency on the weak link (power-save off, max TX power).
3. Make the next event diagnosable/alertable from ioBroker.
4. Keep the recovery-timing logic host-testable, per repo convention.

## Non-goals (deferred)

- **Static IP / skip DHCP** — did not cause this incident (a zombie association,
  not slow DHCP); adds per-deployment config and a new failure mode (breaks on
  subnet/lease change).
- **BSSID pinning** — prevents roaming/failover if that radio dies.
- **MQTT backoff redesign** — LWT, clean session, and a bounded fixed retry
  cadence are already in place (`src/main.cpp:218`). The root cause is the WiFi
  layer, not MQTT backoff.

## Design

### 1. WiFi one-time hardening (`setup()`)

Add, once, before the first connect:

- `WiFi.setSleep(false)` — disable modem power-save.
- `WiFi.setTxPower(WIFI_POWER_19_5dBm)` — max TX power.
- `WiFi.setAutoReconnect(true)` — let the driver self-heal between our checks.
- `WiFi.persistent(false)` — keep WiFi-config NVS writes off the hot path.

Also `s_mqtt.setSocketTimeout(4)` (4 s) so a dead-link `connect()` fails fast
instead of blocking ~15 s under the 30 s task-WDT.

### 2. New pure module `net_watchdog` (host-testable)

`src/net_watchdog.h` / `src/net_watchdog.c` — no WiFi/Arduino headers, mirrors
`energy_reset`. Stateless decision function:

```c
typedef enum {
    NET_ACTION_NONE,        // healthy or still within grace
    NET_ACTION_REASSOCIATE, // force WiFi teardown + reconnect
    NET_ACTION_RESTART      // ESP.restart()
} net_action_t;

typedef struct {
    uint32_t reassoc_after_ms;     // e.g. 120000
    uint32_t restart_after_ms;     // e.g. 300000
    uint32_t reassoc_interval_ms;  // e.g. 30000 (throttle between reassoc attempts)
} net_watchdog_cfg_t;

net_action_t net_watchdog_decide(uint32_t now_ms,
                                 uint32_t last_healthy_ms,
                                 uint32_t last_reassoc_ms,
                                 const net_watchdog_cfg_t *cfg);
```

Logic (all `uint32_t`, wraparound-safe subtraction — `now - last_healthy` is
correct across the 49.7-day `millis()` rollover as long as the elapsed span is
< 49.7 days, which it always is here):

- `elapsed = now_ms - last_healthy_ms`
- `elapsed >= restart_after_ms` → `NET_ACTION_RESTART`
- else if `elapsed >= reassoc_after_ms` **and**
  `(now_ms - last_reassoc_ms) >= reassoc_interval_ms` → `NET_ACTION_REASSOCIATE`
- else → `NET_ACTION_NONE`

The `reassoc_interval_ms` throttle prevents hammering `WiFi.begin()` on every
~25 s loop while waiting out the restart threshold.

**Config values** (compile-time constants in `config.h`):
`NET_REASSOC_AFTER_MS = 120000`, `NET_RESTART_AFTER_MS = 300000`,
`NET_REASSOC_INTERVAL_MS = 30000`.

Add `+<net_watchdog.c>` to `[env:native]` `build_src_filter` in
`platformio.ini`. Tests in `test/test_net_watchdog/`.

### 3. Glue in `main.cpp`

State: `s_last_healthy_ms`, `s_last_reassoc_ms`, `s_wifi_reconnect_count`,
`s_mqtt_reconnect_count` (all RAM-only — a watchdog `ESP.restart()` plus
`reset_reason=sw` on the next boot is itself the durable signal).

Per loop, near the top (after `esp_task_wdt_reset()`):

1. If `s_mqtt.connected()` → `s_last_healthy_ms = millis()`.
2. Call `net_watchdog_decide(millis(), s_last_healthy_ms, s_last_reassoc_ms, &cfg)`:
   - `NET_ACTION_REASSOCIATE` → `WiFi.disconnect(true, true)`; `WiFi.begin(...)`;
     `s_wifi_reconnect_count++`; `s_last_reassoc_ms = millis()`. Log it.
   - `NET_ACTION_RESTART` → log, best-effort final diag/serial flush, then
     `ESP.restart()`.
   - `NET_ACTION_NONE` → continue as normal.

Initialize `s_last_healthy_ms = millis()` in `setup()` so boot starts with a
full grace window, then update it each loop while MQTT is connected. This means
a boot that never reaches MQTT within `restart_after_ms` will itself trigger a
watchdog restart — which is the desired behavior for a wedged bring-up.

`wifi_connect()` keeps its fast path but no longer *owns* recovery — the
watchdog does. `mqtt_connect()` bumps `s_mqtt_reconnect_count` on each
successful (re)connection after a disconnect.

### 4. Observability

New retained `diag/` topics (published in `publish_diag()`):

- `diag/wifi_reconnect_count` — forced-reassociation count this boot.
- `diag/mqtt_reconnect_count` — successful MQTT (re)connects this boot.
- `diag/mqtt_disconnect_s` — `(millis() - s_last_healthy_ms) / 1000`; 0 while
  healthy. Lets ioBroker alert on a growing gap.

## Testing

- **Host (`pio test -e native -f test_net_watchdog`)**: boundary conditions
  (just below / at / above each threshold), reassoc throttle (no second
  REASSOCIATE within `reassoc_interval_ms`, yes after), RESTART precedence over
  REASSOCIATE, `millis()` wraparound (`now < last_healthy` numerically).
- **On hardware**: flash, capture serial with `tools/capture_serial.py`, confirm
  normal steady-state has zero spurious reassoc/restart, and that a forced AP
  outage triggers reassociate at ~2 min and reboot at ~5 min. Confirm the three
  new `diag/` topics publish.

## Worst-case guarantee

Any silent state is bounded to ~5 min: unhealthy → reassociate at 2 min →
reboot at 5 min → fresh boot re-runs full WiFi/MQTT bring-up. `reset_reason=sw`
on the next boot flags a watchdog-driven restart for after-the-fact diagnosis.
