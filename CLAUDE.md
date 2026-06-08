# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 firmware (PlatformIO + Arduino) that bridges a Hoymiles HMS-800-2WB
micro-inverter to MQTT: connect over BLE, poll live data every 30 s, publish
one MQTT topic per metric over WiFi.

## Commands

`pio` is **not** on PATH — invoke PlatformIO through the Python module.

```bash
# Native unit tests (pure-logic modules: crypto + frame), run on the host
python3 -m platformio test -e native

# Run a single native test suite
python3 -m platformio test -e native -f test_crypto   # or -f test_frame

# Build firmware
python3 -m platformio run -e esp32

# Flash + serial monitor (device connected over USB; 115200 baud)
python3 -m platformio run -e esp32 -t upload
python3 -m platformio device monitor -b 115200

# Regenerate nanopb C from proto/ (after editing a .proto / .options file)
nanopb_generator proto/<Name>.proto -D src/proto
```

**Native build is macOS/Homebrew-specific.** `platformio.ini`'s `[env:native]`
hardcodes an mbedTLS include/lib path (`/opt/homebrew/Cellar/mbedtls@3/...`).
On a different machine or mbedTLS version, update those `-I`/`-L` paths.

## Testing strategy (important)

Only `crypto` and `frame` are testable on the host — `[env:native]`'s
`build_src_filter` deliberately compiles **only** `crypto.cpp` and `frame.cpp`,
because every other module pulls in NimBLE / nanopb / Arduino headers that don't
exist off-device. `ble_client`, `handshake`, and `poller` are verified by
flashing real hardware and watching the serial log + MQTT topics. There is no
host-side coverage of the BLE state machine, so changes there can only be
proven on a real inverter.

Crypto test vectors in `test/test_crypto/` were generated from the
`hoymiles-wifi` Python reference and are validated against real mbedTLS — trust
them as regression anchors.

## Architecture

Strictly layered, single-responsibility modules. The dependency direction never
inverts: orchestration (`handshake`, `poller`, `main`) depends on primitives
(`crypto`, `frame`, `ble_client`), never the reverse.

- **`crypto`** — `triple_sha256`, CRC16-Modbus, and the two cipher suites. Pure
  C, mbedTLS only, zero BLE/Arduino knowledge.
- **`frame`** — packs/parses the `HM` wire frame (header + ciphertext + optional
  GCM tag). Knows bytes, not crypto.
- **`ble_client`** — NimBLE GATT: scan/connect/MTU/TX-write/RX-notify. The RX
  notify callback runs on the **NimBLE task (core 0)** while the rest runs on the
  main loop (core 1) — cross-core handoff uses an `std::atomic` callback pointer
  plus `volatile` RX-accumulation state.
- **`handshake`** — obtains `encRand` (V0 pairing or NVS load), then CommCmd
  login + time-sync. Persists `encRand` to NVS (`Preferences`).
- **`poller`** — RealDataNew request → reassemble paged BLE notifications →
  nanopb decode → scale → MQTT publish. Paged responses are all-or-nothing.
- **`main`** — `setup`/`loop` state machine: WiFi → BLE+handshake → MQTT → poll,
  with reconnect on each layer and a task watchdog. BLE is discovered before MQTT
  because the MQTT topics/Last-Will embed the runtime-discovered serial.

### Protocol invariants (get these wrong and nothing decrypts)

- **Two cipher generations.** V0 = AES-128-CBC + PKCS7, used **once** to pair
  and extract `encRand`. V1 = AES-128-GCM, used for every normal request after
  pairing. Key/IV/nonce derivation is `triple_sha256`-based — see the wire
  reference in `docs/superpowers/specs/`.
- **Endianness flips between layers.** Wire-frame fields (`cmd`, `tid`, `crc`,
  `length`) are **big-endian**. The V1 GCM nonce and AAD pack `cmd`/`tid` as
  **little-endian**. The native tests encode this distinction explicitly.
- **CRC16 covers ciphertext only** — never the appended GCM tag.
- **Ciphertext length comes from the frame's `length` field**, not the number of
  bytes accumulated off BLE (trailing notification bytes would otherwise shift
  the tag pointer).

### Watchdog constraint

The 30 s task watchdog (`esp_task_wdt`) must be fed during every long wait. The
poll-interval idle is sliced (`WDT_FEED_SLICE_MS`) rather than one big `delay()`,
and the WiFi-connect loop feeds it too. A single `delay(POLL_INTERVAL_MS)` would
equal the timeout and panic-reset every cycle.

### Generated code

`src/proto/*.pb.{c,h}` are nanopb-generated and committed. Don't hand-edit them;
edit the `proto/*.proto` / `proto/*.options` source and regenerate. The
`.options` files cap repeated/string field sizes for the embedded target.

## Configuration / secrets

Split into two headers:
- **`src/secrets.h`** — gitignored, per-deployment secrets (`WIFI_SSID`,
  `WIFI_PASSWORD`, `MQTT_HOST`). Created by copying `src/secrets.example.h`.
  A fresh clone won't compile until this exists.
- **`src/config.h`** — versioned, non-secret tunables (MQTT port/client-id, poll
  interval, retry/timeout windows). It `#include`s `secrets.h`.

The inverter serial and MQTT topic prefix are **derived at runtime**: the firmware
scans for a BLE advertisement whose name starts with `BLE_NAME_PREFIX` (`"RMI-"`),
extracts the 12-char serial tail, and builds all topics as
`MQTT_TOPIC_PREFIX + <discovered-serial> + "/"`. Both prefix constants live in
`config.h`. If several inverters are in BLE range, set `INVERTER_SN_FILTER` in
`config.h` to the exact 12-char serial tail to pin one device; leave it empty to
connect to the first `RMI-` advertisement found.
