# BLE Serial Auto-Discovery — Design Spec

**Date:** 2026-06-08
**Status:** Approved

## Goal

Let the firmware discover the inverter's serial number automatically from its
BLE advertisement, so the same compiled binary can run on any Hoymiles
HMS-800-2WB without per-device edits to `secrets.h`. Today `BLE_DEVICE_NAME` and
`INVERTER_SN` are compile-time `#define`s that must be set by hand before every
flash.

## Background

The inverter advertises over BLE as `RMI-<12-char-serial>` (e.g.
`RMI-AABBCCDDEE12`). That serial tail is the device identity and is used in three
places:

1. **BLE scan match** — `ble_client` currently does an exact name match against
   `BLE_DEVICE_NAME`.
2. **V0 crypto derivation** — `v0_derive_key`/`v0_derive_iv` (and the CommCmd
   `dtu_sn`) take the serial string; it must be byte-exact or pairing fails.
3. **MQTT topic prefix** — `MQTT_BASE_TOPIC = "hoymiles/" INVERTER_SN "/"`, a
   compile-time string concatenation used throughout `poller` and `main`.

Because the serial is broadcast in the advertisement, items 2 and 3 can be
derived from item 1 at runtime — eliminating the manual config entirely.

## Design

### 1. BLE scan: prefix match + serial extraction

`ble_connect` changes from exact-match to prefix-match and reports the
discovered serial back to the caller.

New signature (`ble_client.h`):

```c
// Scans for a device whose advertised name starts with name_prefix. If
// sn_filter is non-NULL and non-empty, additionally requires the serial tail
// (the name after the prefix) to equal sn_filter exactly. On success, copies
// the matched serial tail into sn_out (sn_out_len must be >= 13), connects,
// negotiates MTU, and subscribes to RX notifications.
bool ble_connect(const char *name_prefix, const char *sn_filter,
                 char *sn_out, size_t sn_out_len, BleRxCallback rx_cb);
```

Scan-loop matching logic:
- Read the advertised name.
- Match when `name` starts with `name_prefix` AND `strlen(name) > strlen(prefix)`.
- If `sn_filter` is non-empty, require the tail to `strcmp`-equal `sn_filter`;
  otherwise accept the first prefix match.
- On match: copy the tail (null-terminated, bounded by `sn_out_len`) into
  `sn_out`, record the address, stop scanning.

The discovered tail is the only source of truth for the serial — no separate
config value can drift out of sync with it.

### 2. Serial threaded as a runtime parameter

`handshake_run` takes the serial explicitly:

```c
bool handshake_run(const char *sn, uint16_t *tid, uint8_t enc_rand_out[16]);
```

`do_v0_pairing` and `do_commcmd` take `const char *sn` and use it in place of
the `INVERTER_SN` macro (`v0_derive_key(sn, …)`, `v0_derive_iv(…, sn, …)`,
`strncpy(cmd_msg.dtu_sn, sn, …)`). No other handshake logic changes.

### 3. MQTT topics built at runtime

The compile-time `MQTT_BASE_TOPIC` macro is removed. `config.h` keeps a
non-secret `MQTT_TOPIC_PREFIX "hoymiles/"`. The base topic
`"<prefix><sn>/"` is built once after discovery in `main` and:

- passed into `poller_poll(mqtt, base_topic, tid, enc_rand)`; the poller's
  `publish_float`/`publish_str` helpers prepend a file-static base pointer set
  at the top of `poller_poll`.
- used in `main` to build the status (LWT) and `firmware_version` topics.

### 4. State-machine reorder (BLE before MQTT)

The serial is unknown until the BLE scan, but the MQTT Last-Will/status topic
(`hoymiles/<sn>/status`) needs it at connect time. So the `loop()` order becomes:

```
WiFi connect
  → (if not connected/paired) BLE connect + handshake   ← discovers SN, builds topics
  → MQTT connect (LWT = runtime status topic; publishes online + firmware_version)
  → poll
  → sliced idle wait (watchdog fed)
```

Reconnect semantics:
- The discovered SN and built topics are cached in `main` statics across loop
  iterations (a given device's serial never changes within a session).
- MQTT drop → reconnect MQTT only (topics already known from the cached SN).
- BLE drop → reconnect BLE + re-handshake (re-discovers the same SN).
- The existing consecutive-failure NVS-wipe logic is unchanged.

### 5. Configuration changes

`secrets.h` / `secrets.example.h` (gitignored secret values) shrink to the
genuine secrets only:

```c
#define WIFI_SSID       "..."
#define WIFI_PASSWORD   "..."
#define MQTT_HOST       "..."
```

`BLE_DEVICE_NAME` and `INVERTER_SN` are **removed**. `config.h` (versioned)
gains:

```c
#define BLE_NAME_PREFIX     "RMI-"     // inverter BLE advertisement prefix
#define MQTT_TOPIC_PREFIX   "hoymiles/"
#define INVERTER_SN_FILTER  ""          // empty = first RMI- device; set to a
                                        // 12-char tail to pin one of several
```

## Components touched

| File | Change |
|------|--------|
| `src/ble_client.h/.cpp` | Prefix-match scan, serial extraction, new `ble_connect` signature |
| `src/handshake.h/.cpp` | `handshake_run(sn, …)`; `do_v0_pairing`/`do_commcmd` take `sn` |
| `src/poller.h/.cpp` | `poller_poll(…, base_topic, …)`; runtime topic prefix |
| `src/main.cpp` | Discover SN, build runtime topics, reorder BLE-before-MQTT |
| `src/config.h` | Add `BLE_NAME_PREFIX`, `MQTT_TOPIC_PREFIX`, `INVERTER_SN_FILTER`; drop `MQTT_BASE_TOPIC` |
| `src/secrets.h` / `secrets.example.h` | Drop `BLE_DEVICE_NAME`, `INVERTER_SN` |
| `CLAUDE.md`, `README.md` | Document auto-discovery; update config tables |

`crypto` and `frame` are untouched — their APIs already take the serial as a
runtime `const char *`, so the native test suite is unaffected.

## Error handling

- **No `RMI-` device found** → `ble_connect` returns false; `main` retries after
  `BLE_RETRY_MS` (existing behavior).
- **`sn_filter` set but no match** → treated as "not found"; retry.
- **`sn_out` too small** → serial is truncated to fit and null-terminated; with a
  ≥13-byte buffer this never triggers for a well-formed 12-char tail.
- Multiple inverters in range with empty filter → first match wins
  (deterministic only up to scan ordering; documented, and `INVERTER_SN_FILTER`
  is the escape hatch).

## Testing

- **Native:** unchanged (`crypto`, `frame`); 15/15 must still pass. Serial
  extraction lives inside the NimBLE scan loop and is not host-testable by
  design, consistent with the rest of `ble_client`.
- **Build:** `pio run -e esp32` must succeed.
- **On device:** flash with placeholder-free `secrets.h` (no SN), confirm the
  serial log prints the discovered `Inverter SN:` and that
  `hoymiles/<discovered-sn>/…` topics publish.

## Out of scope

- Runtime WiFi/MQTT provisioning (captive portal).
- Persisting the discovered SN to NVS (re-discovery on each connect is cheap).
- Supporting non-`RMI-` device families.
