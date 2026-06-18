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
2. **Login + time-sync** over the encrypted (AES-128-GCM) command channel. The
   DTU whitelists clients by a `bleId`; a new one is authorised once with the
   inverter's BLE PIN (see [Configure](#configure)), after which login is silent.
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
| `MQTT_USER` / `MQTT_PASSWORD` | Broker credentials — leave empty (`""`) for anonymous |
| `BLE_PIN` | The inverter's BLE PIN from the S-Miles app. Needed **once** to whitelist this client; leave empty if the device has no PIN |
| `BLE_ID` *(optional)* | A stable client identity. Defaults to a placeholder in `config.h` that works fine for a single inverter |

The firmware finds the inverter automatically: it scans for a BLE device whose
name starts with `RMI-` and reads the 12-char serial tail from the advertisement.
No manual serial entry is required. If several Hoymiles inverters are in BLE range,
set `INVERTER_SN_FILTER` in `src/config.h` to the exact 12-char serial tail of the
one you want to pin.

**First-time pairing & the BLE PIN.** The DTU only accepts a *whitelisted* client.
On first run the firmware presents its `BLE_ID` and, if the device asks for a PIN
(`sts=3`), submits `BLE_PIN` once to get whitelisted — afterwards `BLE_ID` alone
logs in. A **wrong** PIN that is retried will lock the DTU for ~11 minutes, so the
firmware submits a PIN at most once per boot and never repeats a rejected one. If
you're unsure of the PIN, validate it first with the host-side
[reference harness](ble-test/README.md) rather than guessing on the device.

Non-secret tunables (`POLL_INTERVAL_MS`, retry/timeout intervals, `BLE_NAME_PREFIX`,
`MQTT_TOPIC_PREFIX`, `INVERTER_SN_FILTER`, `BLE_ID`/`BLE_PIN` fallbacks) live in
the versioned `src/config.h`, which `#include`s `secrets.h`.

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

A healthy boot log walks through: WiFi connect → MQTT connect (so health is
reported immediately) → BLE scan/connect → handshake (`encRand` from NVS or fresh
V0 pairing) → `[PL] Published …` every 30 s. When the inverter is asleep the BLE
step keeps failing (`diag/ble_state=searching`) but MQTT stays connected.

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
| `energy_today` | kWh | sum of per-panel daily energy |
| `energy_total` | kWh | sum of panel lifetime totals |
| `last_seen` | epoch s | timestamp of last publish |
| `firmware_version` | — | published on MQTT connect |
| `status` | `online`/`offline` | retained; **bridge** liveness — `online` once the ESP32 is on the broker, `offline` is the MQTT Last-Will (ESP32 down). *Not* inverter production: at night the bridge stays `online` while the inverter sleeps |
| `diag/reset_reason` | — | cause of the last reboot: `poweron`, `brownout`, `task_wdt`, `panic`, … (retained) |
| `diag/uptime_s` | s | seconds since boot — resets to ~0 mean the device is rebooting |
| `diag/free_heap` / `diag/min_free_heap` | bytes | current / lowest-ever free heap; a steady decline signals a leak |
| `diag/wifi_rssi` | dBm | WiFi signal strength |
| `diag/ble_state` | — | `searching` (inverter asleep / out of range), `ready`, `polling`, `poll_fail`, `wifi_down` |

The `diag/*` topics are published every cycle **even while the inverter is asleep**,
because MQTT is kept connected independently of BLE. That makes a headless device
diagnosable remotely: if it stops waking at sunrise, `diag/reset_reason` and
`diag/uptime_s` tell you whether it brown-outs, watchdog-resets, or hangs.

## Troubleshooting

- **`No device matching 'RMI-*' found`** — inverter out of BLE range, or asleep (no PV
  input at night → radio off), or another BLE client (the S-Miles app, a test
  script) holds the inverter's single connection slot. If multiple inverters are
  nearby, set `INVERTER_SN_FILTER` in `config.h` to the exact serial tail.
- **`login poll: sts=3` then polls time out** — `BLE_ID` isn't whitelisted and no
  (or a wrong) `BLE_PIN` is set. Set the correct `BLE_PIN` in `secrets.h`, flash
  once to whitelist, then it logs in with `sts=1`. Don't brute-force the PIN — a
  few wrong tries lock the DTU ~11 minutes (verify it with the
  [reference harness](ble-test/README.md) first).
- **`Proto decode failed: wrong wire type`** — the DTU's firmware sent a field
  whose type differs from the `.proto`. nanopb is strict; drop or retype the
  offending field in `proto/*.proto` and regenerate (see CLAUDE.md).
- **`GCM auth tag failure` / decrypt errors after working before** — a stale
  pairing. After enough consecutive failures the firmware clears NVS and re-pairs
  automatically; a power-cycle also forces a fresh pairing attempt.
- **Boot-loops / watchdog resets** — usually a blocking WiFi/MQTT connect; check
  credentials and broker reachability in the serial log.
- **Stops publishing overnight and never wakes at sunrise** — the usual cause is
  a **stale pairing**: the DTU forgets its `encRand` when it powers down at night,
  so the next morning the cached secret is rejected (BLE connects, then the DTU
  drops the link mid-login — `diag/ble_state` cycles `ready`→`searching`). The
  firmware now detects a handshake that fails *with a live BLE link* and re-pairs
  (V0) automatically after a couple of tries. If it still won't come up, check the
  other `diag/` fields: `reset_reason=brownout` points at an under-spec USB supply
  sagging under RF load; a climbing-then-crashing `uptime_s` or falling
  `min_free_heap` points at a resource leak. The bridge keeps MQTT (and `diag/*`)
  alive while the inverter sleeps precisely so this is visible the next morning.

## Project layout

```
src/        firmware modules (crypto, frame, ble_client, handshake, poller, main)
src/proto/  nanopb-generated protobuf decoders (do not hand-edit)
proto/      .proto schemas + .options size caps (regenerate src/proto from these)
test/       native Unity tests for crypto + frame
tools/      capture_serial.py — headless serial-log capture
ble-test/   host-side reference harness (hiflow-ble) for protocol ground truth
docs/       design specs and implementation plans
```

See [CLAUDE.md](CLAUDE.md) for architecture details and protocol invariants.

## Acknowledgements

The BLE protocol was reverse-engineered with help from two excellent reference
projects, which this firmware was validated against:

- [`hoymiles-wifi`](https://github.com/suaveolent/hoymiles-wifi) — the TCP/local
  protocol and protobuf message definitions.
- [`hiflow-ble`](https://github.com/TheTiEr/hiflow-ble) — the BLE transport,
  V0/V1 crypto, and CommCmd handshake (used as the host-side oracle in
  [`ble-test/`](ble-test/)).

## License

[MIT](LICENSE) © Phillip Nientiedt

> **Disclaimer:** Not affiliated with or endorsed by Hoymiles. "Hoymiles" and
> "S-Miles" are trademarks of their respective owner. Use at your own risk;
> interacting with the inverter over BLE is unofficial.
