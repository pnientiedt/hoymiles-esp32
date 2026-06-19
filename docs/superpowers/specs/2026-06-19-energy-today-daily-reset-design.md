# Daily `energy_today` zero-reset — Design

**Date:** 2026-06-19
**Status:** Approved (pending spec review)

## Problem

The inverter's BLE radio is off all night, so the bridge stops polling. Because
every MQTT topic is published **retained**, the last `energy_today` value from the
previous evening stays on the broker untouched until the inverter wakes the next
morning and the first poll overwrites it. Consumers (dashboards, Home Assistant,
automations) therefore read a stale, non-zero "today's energy" through the whole
night and early morning — a false report.

Two cases must be fixed:

1. **Day rollover while running** — at local midnight, `energy_today` should drop
   to `0` even though the inverter is asleep and no poll is happening.
2. **Stale value after a (re)boot** — if the firmware restarts and the retained
   `energy_today` was produced on a previous day, it should publish `0` rather
   than leave yesterday's figure standing.

## Key insight

Both cases are the **same rule**. Each reduces to: *"if the local calendar day of
the last energy publish is not today, publish `0`."* They differ only in what the
"last publish day" is compared against, and that is solved by persisting one fact.

## Approach (chosen)

Persist a single value in NVS — **`eday`**, the local-calendar-day key of the last
energy publish — and drive both cases from it. (Alternative approaches considered
and rejected below.)

### The rule

On each `loop()` iteration, **after** WiFi/NTP time is valid and MQTT is connected
and topics are built:

```
today = local_day_key(now)            // 0 if time not yet synced
if eday != 0 and today != 0 and eday != today:
    publish energy_today = 0          (retained)
    for each cached pv port n:
        publish pv/<n>/energy_today = 0   (retained)
    eday = today                      // persist to NVS
```

A **successful poll** also advances `eday` to `today` (persisted only when it
changes). This means the morning's first real reading "claims" the day, so the
explicit zero will not re-fire, and the retained `0` published overnight is simply
overwritten by the inverter's new (small) daily value when it wakes.

### Why this satisfies both requirements

- **Midnight rollover**: `loop()` ticks roughly every 30 s even while the inverter
  is asleep (the BLE-retry path still `idle_wait`s and returns), so the day change
  is detected within one idle interval after local midnight.
- **Startup staleness**: the first `loop()` iteration after a (re)boot runs the
  identical check. `eday` loaded from NVS still holds *yesterday*, so it zeroes.
  No separate startup code path is needed — the loop's check is the startup check.

### What is NOT reset

- **`energy_total`** — lifetime cumulative; must never be zeroed.
- **`last_seen`** — intentionally left stale; its staleness is a useful signal that
  the inverter has not been contacted recently.
- **`ac/*`, `pv/<n>/{voltage,current,power}`** — out of scope; staleness of these
  is already conveyed by `diag/ble_state` and the architecture's health model.

## Time / timezone

The clock is currently UTC (`configTime(0, 0, "pool.ntp.org")`). The inverter
resets its own daily energy at *local* midnight, so the boundary must be local.

- **`src/config.h`**: add a POSIX TZ string, non-secret, documented to be set
  per-deployment:
  ```c
  // POSIX TZ string defining the local day boundary for the energy_today reset.
  // Example (Central Europe with DST). Set to your own zone.
  #define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
  ```
- **`src/main.cpp`**: replace `configTime(0, 0, "pool.ntp.org")` with
  `configTzTime(TIMEZONE, "pool.ntp.org")` — this sets `TZ`, runs `tzset()`, and
  starts SNTP in one call.

**Invariant preserved:** `time_t` epoch is always UTC regardless of `TZ`; only
`localtime_r`/`gmtime_r` conversions differ. `handshake.cpp`'s time-sync
(`action=104`) uses `gmtime_r` and still receives UTC — it is unaffected by this
change. Only the new local-day computation uses `localtime_r`.

## Per-panel ports (cached in NVS)

To zero `pv/<n>/energy_today` while the inverter is offline (overnight, and after
a morning reboot before the first poll), the firmware must know the panel port
numbers without a live decode.

- **`poller_poll`** gains a nullable out-parameter that reports the discovered port
  numbers and count on a successful poll:
  ```c
  bool poller_poll(PubSubClient &mqtt, const char *base, uint16_t *tid,
                   const uint8_t enc_rand[16],
                   uint8_t *out_ports, uint8_t *out_port_count); // both nullable
  ```
  Existing callers may pass `nullptr`/`nullptr`.
- **`main.cpp`** persists the list to NVS key **`pvports`** (a raw byte blob, one
  byte per port; the count is the blob length returned by `getBytes`) whenever it
  changes, and loads it at boot into a small RAM cache. The reset uses this cached
  list to zero exactly the panels that exist.
- **First-ever boot** with no `pvports` cached: only the top-level `energy_today` is
  zeroed; per-panel topics do not exist yet anyway. After the first successful poll
  seeds `pvports`, subsequent rollovers zero the panels too.

Bound the cached array to a small fixed maximum (e.g. `MAX_PV_PORTS = 8`) to keep
the NVS blob and RAM cache fixed-size.

## Pure module + host testing

Per the project's testing strategy (pure logic is unit-tested on the host; BLE /
Arduino paths are verified on hardware), extract the date logic into a new
**Arduino-free** module so it gets real host coverage.

### `src/energy_reset.{h,cpp}`

```c
// Equality key for a local calendar day. Returns 0 when time is unknown
// (caller passes a tm derived from an unsynced clock as the sentinel case).
uint32_t local_day_key(const struct tm *lt);

// True when a reset is due: both keys known and different.
bool energy_day_changed(uint32_t stored, uint32_t today);
```

- `local_day_key` builds an equality key from local broken-down time
  (`tm_year`, `tm_yday`) — it need not be monotonic, only collision-free across
  distinct days (same year+yday ⇒ same day; otherwise differ). Returns `0` for the
  unknown/unsynced sentinel.
- `energy_day_changed` is the trivial decision, factored out so the year-rollover
  and unknown-sentinel behavior is locked by tests.

`main.cpp` keeps all the NVS, MQTT, `localtime_r`, and Arduino wiring (not
host-testable; verified on hardware).

### `test/test_energy_day/`

Host suite (added to `[env:native]`) covering:

- same local day ⇒ no reset;
- next day ⇒ reset;
- **Dec 31 → Jan 1** year boundary ⇒ reset (guards naive yday-only comparison);
- unknown/unsynced key (`0`) on either side ⇒ no reset (no false zero).

### `platformio.ini`

- Add `src/energy_reset.cpp` to `[env:native]`'s `build_src_filter`.
- The new test suite is discovered under `test/test_energy_day/`.

## Control flow (where it hooks in `loop()`)

Insert the reset check **after** `mqtt_connect()` (need MQTT up to publish) and
`ensure_topics()` (need `s_base_topic`), **before** the BLE/poll steps:

```
loop():
  wifi_connect()        // also runs configTzTime + waits for NTP
  ensure_topics()
  mqtt_connect()
  maybe_reset_energy_today()     // <-- NEW: the rule above
  ... BLE connect / handshake ...
  if poller_poll(..., out_ports, &out_n):
        cache/persist pvports if changed
        advance eday to today if changed
  publish_diag()
  idle_wait(POLL_INTERVAL_MS)
```

At cold boot the first iteration runs `wifi_connect` (which now syncs NTP via
`configTzTime`), then `maybe_reset_energy_today()` sees a valid clock — so the
startup stale-check is just the first iteration of the normal loop.

## NVS keys (namespace `hoymiles`, alongside `sn` / `encRand`)

| Key        | Type            | Meaning                                            |
|------------|-----------------|----------------------------------------------------|
| `eday`     | `uint32`        | local-day key of last energy publish (`0`=unknown) |
| `pvports`  | bytes           | discovered PV port numbers, one byte each (len=count) |

Writes occur only when a value actually changes (~1/day for `eday`; rarely for
`pvports`). NVS also elides writes when the stored value is identical, so flash
wear is negligible.

## Edge cases

| Situation                                   | Behavior                                                              |
|---------------------------------------------|----------------------------------------------------------------------|
| NTP not yet synced at boot                   | `today` key = `0` ⇒ skip reset (no false zero) until time syncs.      |
| Multiple reboots in one day                  | `eday` already == today ⇒ no repeat zeroing.                          |
| Inverter still producing past local midnight | Brief `energy_today=0` flicker, then next poll publishes new-day value (matches the inverter's own midnight reset). |
| First-ever boot (no `eday`/`pvports`)        | No reset (nothing stale exists); first poll seeds both keys.          |
| DST transition                               | `localtime_r` applies the offset; yday-based key remains correct.     |
| Timezone misconfigured                       | Still resets once per day, but at the wrong wall-clock minute. Documented to set `TIMEZONE`. |

## Files touched

- `src/config.h` — add `TIMEZONE` (and `MAX_PV_PORTS`).
- `src/main.cpp` — `configTzTime`; load `eday`/`pvports` at boot; `maybe_reset_energy_today()`; call in `loop()`; advance `eday`/`pvports` on successful poll.
- `src/poller.{h,cpp}` — nullable out-param reporting discovered ports on success.
- `src/energy_reset.{h,cpp}` — **new** pure module (`local_day_key`, `energy_day_changed`).
- `test/test_energy_day/` — **new** host test suite.
- `platformio.ini` — add `energy_reset.cpp` to the native `build_src_filter`.

## Alternatives considered

- **Read retained values back from MQTT** (subscribe to `energy_today`/`last_seen`,
  compare `last_seen`'s day): rejected. The MQTT client is publish-only today
  (no `setCallback`/`subscribe`), and MQTT stays connected all night (decoupled
  from BLE), so there is no reconnect at midnight to trigger a re-read — the
  runtime rollover still needs a clock-driven check regardless. The read-back
  would only serve the startup case, at the cost of a net-new async receive path.
- **In-RAM day tracking only**: rejected. Loses state across reboots, so it cannot
  satisfy the startup staleness requirement. It is effectively the chosen approach
  minus the NVS persistence.
- **Config `NUM_PV_PORTS` (assume ports 1..N)** instead of caching: rejected in
  favor of caching discovered ports, for robustness against non-`1..N` port
  numbering.

## Out of scope

- Resetting `energy_total`, `last_seen`, or instantaneous `ac/*` / `pv/*` metrics.
- Home Assistant MQTT auto-discovery / device classes.
- Any change to the BLE handshake or poll cadence.
