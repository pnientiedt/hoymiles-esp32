# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **⚠️ THIS IS A PUBLIC REPOSITORY.** Never commit private data — not in source,
> not in docs, not in tests, not in example/log files, not in commit messages.
> This includes WiFi/MQTT credentials, broker hostnames or LAN IPs, the BLE PIN,
> the inverter serial / `bleId`, `encRand`, and any other per-deployment secret.
> Real values live only in gitignored `src/secrets.h` and the local `ble-test/`
> harness (which reads them from CLI args / env vars, never hardcoded). When
> adding tests or examples, use placeholders. Before any commit, double-check the
> diff for leaked secrets.

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

# Regenerate nanopb C from proto/ (after editing a .proto / .options file).
# nanopb_generator isn't on PATH — use the copy vendored by the Nanopb lib
# (present after a build), and pass -I proto so imports resolve.
python3 .pio/libdeps/esp32/Nanopb/generator/nanopb_generator.py \
    proto/<Name>.proto -D src/proto -I proto
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
- **`handshake`** — obtains `encRand` (V0 pairing or NVS load), then runs the V1
  CommCmd handshake: login (`action=64`, `data=BLE_ID`) → poll status → if the
  DTU asks for a PIN (`sts=3`), submit `BLE_PIN` once (`action=82`) → time-sync
  (`action=104`). CommCmd send cmd is `0xA318`, status `0xA319`; the DTU replies
  on `sent_cmd - 0x0100`. The CommCmd messages are hand-rolled protobuf (the
  upstream `.proto` had the wrong field numbers). Persists `encRand` to NVS
  (`Preferences`).
- **`poller`** — RealDataNew request → reassemble paged BLE notifications →
  nanopb decode → scale → MQTT publish. Paged responses are all-or-nothing.
- **`main`** — `setup`/`loop` state machine: WiFi → MQTT → BLE+handshake → poll,
  with reconnect on each layer and a task watchdog. **MQTT is kept connected
  independently of BLE** so the bridge reports health (and a `diag/` namespace:
  `reset_reason`, `uptime_s`, `free_heap`/`min_free_heap`, `wifi_rssi`,
  `ble_state`) all night while the inverter's radio is off — `status=offline`
  (Last-Will) therefore means the ESP32 itself is down, not "inverter asleep".
  The serial that the MQTT topics/Last-Will embed is discovered over BLE, so it's
  cached in NVS (`sn` key, `hoymiles` namespace) and loaded at boot; before the
  first-ever BLE contact, topics fall back to a chip-ID (`esp32-<mac>`) so health
  still publishes. The long BLE-retry/poll-interval idle waits pump `s_mqtt.loop()`
  as well as the watchdog (`idle_wait`).

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
- **The DTU replies on `sent_cmd - 0x0100`** (e.g. `0xA301`→`0xA201`,
  `0xA311`→`0xA211`) and keys its reply's GCM nonce/AAD on that *reply* cmd —
  derive response crypto from the parsed reply cmd, not the request cmd.
- **Notifications must be armed with an acknowledged CCCD write.** NimBLE's
  `subscribe()` writes the CCCD without response by default; this DTU then never
  enables notifications and stays silent even though data writes are ATT-ACK'd.
  Pass `response=true` (see `ble_client.cpp`). This was the single hardest bug.
- **nanopb is strict about wire types on known fields** (it only skips *unknown*
  field numbers). When the DTU sends a field with a type the `.proto` doesn't
  expect (e.g. `RealDataNewReqDTO` field 13 is a submessage, not `uint64`),
  decode fails with "wrong wire type" — drop or retype that field and regenerate.
  Full-fat protobuf (the Python reference) silently tolerates the same mismatch.

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
  `WIFI_PASSWORD`, `MQTT_HOST`, optional `MQTT_USER`/`MQTT_PASSWORD` for broker
  auth — empty = anonymous, and optional `BLE_PIN`/`BLE_ID` for the CommCmd
  whitelist). Created by copying `src/secrets.example.h`. A fresh clone won't
  compile until this exists.
- **`src/config.h`** — versioned, non-secret tunables (MQTT port/client-id, poll
  interval, retry/timeout windows, and `BLE_ID`/`BLE_PIN` fallbacks used when
  `secrets.h` doesn't set them). It `#include`s `secrets.h`.

The inverter serial and MQTT topic prefix are **derived at runtime**: the firmware
scans for a BLE advertisement whose name starts with `BLE_NAME_PREFIX` (`"RMI-"`),
extracts the 12-char serial tail, and builds all topics as
`MQTT_TOPIC_PREFIX + <discovered-serial> + "/"`. Both prefix constants live in
`config.h`. If several inverters are in BLE range, set `INVERTER_SN_FILTER` in
`config.h` to the exact 12-char serial tail to pin one device; leave it empty to
connect to the first `RMI-` advertisement found.
