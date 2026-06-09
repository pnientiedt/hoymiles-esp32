# Hoymiles ESP32 BLE → MQTT Bridge

Firmware for an ESP32 that connects to a **Hoymiles HMS-800-2WB** micro-inverter
over Bluetooth LE, polls live production data every 30 seconds, and publishes it
to MQTT over WiFi — one topic per metric, retained, with a Last-Will status
topic.

It speaks the inverter's native BLE protocol directly (AES-CBC pairing →
AES-GCM data requests, nanopb-decoded protobuf), so no cloud account or
DTU/S-Miles gateway is required.

## How it works

```
   ESP32                                   Hoymiles HMS-800-2WB
   ┌──────────────────────────┐  BLE GATT  ┌────────────────────┐
   │ WiFi → BLE → MQTT → poll │◄──────────►│   inverter radio   │
   └──────────────┬───────────┘            └────────────────────┘
                  │ WiFi / MQTT
                  ▼
            MQTT broker  ──►  Home Assistant / Node-RED / etc.
```

1. **Pair** (once): AES-128-CBC handshake extracts a per-device `encRand` secret,
   stored in NVS flash so it survives reboots.
2. **Login + time-sync** via the encrypted command channel.
3. **Poll** every 30 s: request live data (AES-128-GCM), reassemble the paged BLE
   response, decode the protobuf, scale the raw values, and publish to MQTT.

If a poll fails it reconnects; the stored pairing is only discarded after
repeated consecutive failures.

## Hardware

- Any ESP32 dev board (`board = esp32dev`; built-in BLE + WiFi).
- Within BLE range of the inverter (~10 m line of sight).
- USB cable for flashing.

## Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/) —
  installed as a Python module here, invoked as `python3 -m platformio`.
- For the host-side unit tests: **mbedTLS 3.x** (macOS: `brew install mbedtls`).

## Configure

Per-deployment secrets live in `src/secrets.h`, which is **gitignored**. Copy
the template and fill it in:

```bash
cp src/secrets.example.h src/secrets.h
# edit src/secrets.h
```

| Setting (in `secrets.h`) | What to put |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Your 2.4 GHz WiFi credentials |
| `MQTT_HOST` | Your MQTT broker address |

The firmware finds the inverter automatically: it scans for a BLE device whose
name starts with `RMI-` and reads the 12-char serial tail from the advertisement.
No manual serial entry is required. If several Hoymiles inverters are in BLE range,
set `INVERTER_SN_FILTER` in `src/config.h` to the exact 12-char serial tail of the
one you want to pin.

Non-secret tunables (`POLL_INTERVAL_MS`, retry/timeout intervals, `BLE_NAME_PREFIX`,
`MQTT_TOPIC_PREFIX`, `INVERTER_SN_FILTER`) live in the versioned `src/config.h`,
which `#include`s `secrets.h`.

## Test

The pure-logic modules (`crypto`, `frame`) have host-side unit tests — run them
without any hardware:

```bash
python3 -m platformio test -e native

# A single suite:
python3 -m platformio test -e native -f test_crypto
python3 -m platformio test -e native -f test_frame
```

> The `native` environment in `platformio.ini` hardcodes a Homebrew mbedTLS
> path. On Linux or a different mbedTLS version, adjust the `-I`/`-L` flags under
> `[env:native]`.

The BLE / handshake / poller / MQTT paths cannot be unit-tested off-device; they
are validated by flashing and observing (see below).

## Build & deploy

```bash
# Compile
python3 -m platformio run -e esp32

# Flash to a connected ESP32 (auto-detects the serial port)
python3 -m platformio run -e esp32 -t upload

# Watch the serial log to confirm operation
python3 -m platformio device monitor -b 115200
```

A healthy boot log walks through: WiFi connect → BLE scan/connect → handshake
(`encRand` from NVS or fresh V0 pairing) → MQTT connect → `[PL] Published …`
every 30 s.

Then verify on the broker — every topic is retained:

```bash
mosquitto_sub -h <MQTT_HOST> -t 'hoymiles/#' -v
```

## MQTT topics

All under `hoymiles/<INVERTER_SN>/`, where `<INVERTER_SN>` is the 12-char serial
tail auto-discovered from the inverter's `RMI-` BLE advertisement:

| Topic | Unit | Notes |
|---|---|---|
| `ac/voltage` | V | grid voltage |
| `ac/frequency` | Hz | |
| `ac/power` | W | current AC output |
| `ac/current` | A | |
| `ac/power_factor` | — | |
| `ac/temperature` | °C | inverter temp |
| `ac/power_limit` | W | active limit |
| `pv/<n>/voltage` | V | per-panel input `n` |
| `pv/<n>/current` | A | |
| `pv/<n>/power` | W | |
| `pv/<n>/energy_today` | kWh | |
| `pv/<n>/energy_total` | kWh | |
| `energy_today` | kWh | DTU daily total |
| `energy_total` | kWh | sum of panel lifetime totals |
| `last_seen` | epoch s | timestamp of last publish |
| `firmware_version` | — | published on MQTT connect |
| `status` | `online`/`offline` | retained; `offline` is the MQTT Last-Will |

## Troubleshooting

- **`No device matching 'RMI-*' found`** — inverter out of BLE range, or asleep (no PV
  input at night → radio off). If multiple inverters are nearby, set
  `INVERTER_SN_FILTER` in `config.h` to the exact serial tail.
- **`GCM auth tag failure` / decrypt errors after working before** — a stale
  pairing. After enough consecutive failures the firmware clears NVS and re-pairs
  automatically; a power-cycle also forces a fresh pairing attempt.
- **Boot-loops / watchdog resets** — usually a blocking WiFi/MQTT connect; check
  credentials and broker reachability in the serial log.

## Project layout

```
src/        firmware modules (crypto, frame, ble_client, handshake, poller, main)
src/proto/  nanopb-generated protobuf decoders (do not hand-edit)
proto/      .proto schemas + .options size caps (regenerate src/proto from these)
test/       native Unity tests for crypto + frame
docs/       design spec and implementation plan
```

See [CLAUDE.md](CLAUDE.md) for architecture details and protocol invariants.
