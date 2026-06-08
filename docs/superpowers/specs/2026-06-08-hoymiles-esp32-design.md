# Hoymiles ESP32 BLE→MQTT Bridge — Design Spec

**Date:** 2026-06-08
**Inverter:** Hoymiles HMS-800-2WB (HiFlow Pro series)
**Protocol:** Bluetooth Low Energy (BLE) — port 10081 is permanently closed on this hardware generation

## Goal

An ESP32 firmware application that connects to the HMS-800-2WB via BLE, polls live inverter data every 30 seconds, and publishes individual MQTT topics over WiFi. The ESP32 sits near the inverter in the garage; the MQTT broker and any consumers run elsewhere on the network.

```
Inverter (BLE) ←→ ESP32 ←→ WiFi ←→ MQTT broker ←→ Pi / consumers
```

## Hardware

- **ESP32 board** with BLE + WiFi and an **external antenna connector** (u.FL / IPEX) — e.g. ESP32-WROOM-32U, ESP32-C3-Mini-1U, or equivalent. A cheap 2.4 GHz pigtail antenna improves range in weak-signal environments.
- **Power:** USB adapter near the inverter, or PoE splitter → USB.

## Toolchain

**PlatformIO** with Arduino framework.

| Library | Purpose |
|---------|---------|
| `h2zero/NimBLE-Arduino` | BLE GATT client |
| `knolleary/PubSubClient` | MQTT |
| `nanopb/nanopb` | Lightweight protobuf decode (C) |
| mbedTLS | AES-128-CBC, AES-128-GCM, SHA-256 — built into ESP32 Arduino SDK |
| `Preferences` | NVS flash storage for `encRand` |

## Project Structure

New repository: `hoymiles-esp32`

```
hoymiles-esp32/
├── platformio.ini
├── src/
│   ├── config.h              — WiFi creds, MQTT host/port, BLE device name, poll interval
│   ├── main.cpp              — setup/loop: WiFi+MQTT reconnect, drives poller
│   ├── crypto.cpp/.h         — triple-SHA256, V0 AES-128-CBC, V1 AES-128-GCM, CRC16-Modbus
│   ├── frame.cpp/.h          — build + parse HM wire frames
│   ├── ble_client.cpp/.h     — NimBLE GATT connect, MTU negotiate, TX write, RX notify
│   ├── handshake.cpp/.h      — V0 pairing (extract encRand), CommCmd login + time-sync
│   ├── poller.cpp/.h         — poll RealDataNew, scale fields, publish MQTT topics
│   └── proto/                — nanopb generated .c/.h
│       ├── RealDataNew.pb.c/.h
│       └── APPInfomationData.pb.c/.h
└── proto/
    ├── RealDataNew.proto
    └── APPInfomationData.proto
```

Each file has one responsibility. `crypto` has no BLE or MQTT knowledge. `frame` only knows bytes. `ble_client` only knows GATT. `poller` orchestrates everything.

## Protocol Background

The HMS-800-2WB (HiFlow Pro series) uses BLE-only for local communication. The TCP port 10081 used by older Hoymiles devices is permanently closed on this hardware. The BLE protocol is identical in framing to the TCP protocol but requires additional steps:

### Wire Frame Format

```
[0:2]    0x484D ("HM" magic)
[2:4]    cmd    big-endian uint16
[4:6]    tid    big-endian uint16 (monotonic transaction ID)
[6:8]    CRC16-Modbus of ciphertext
[8:10]   length = len(ciphertext) + 10  (excludes 16-byte GCM tag)
[10:N]   ciphertext
[N:N+16] AES-128-GCM auth tag (V1 only)
```

### Crypto Modes

**V0 (SN-keyed AES-128-CBC + PKCS7)** — used only for the initial pairing handshake:
- Key: `triple_sha256(sn.encode() + b"Hoymiles@#123456")[:16]`
- IV: `triple_sha256(pack(">HH", cmd, tid) + sn.encode())[16:32]`
- `sn` = 12-character serial tail from BLE advertisement name `RMI-XXXXXXXXXXXX`

**V1 (encRand-keyed AES-128-GCM)** — used for all normal requests:
- Key: `triple_sha256(encRand)[:16]`
- Nonce: `triple_sha256(pack("<HH", cmd, tid) + encRand)[20:32]`
- AAD: `pack("<HH", cmd, tid)`
- `encRand` is a 16-byte per-device secret, flash-fixed, stable across power cycles

**triple_sha256:** SHA-256 applied three times in sequence.

### BLE GATT Layout

- Service UUID: `0000e0ff-3c17-d293-8e48-14fe2e4da212`
- TX characteristic (write): `0000ffe1-0000-1000-8000-00805f9b34fb`
- RX characteristic (notify): `0000ffe2-0000-1000-8000-00805f9b34fb`
- MTU: negotiate 512; chunk writes if needed

### Security Note

The V0 pairing key is derived from the serial number (visible in the BLE advertisement) and a hardcoded salt in the open-source library. Anyone within BLE range (~10–30 m) who knows the library can compute the key and extract `encRand`. This is a protocol-level limitation; the firmware cannot address it.

## Connection Flow

```
Boot
 │
 ├─ Connect WiFi (retry until up)
 ├─ Connect MQTT + register Last Will (status = "offline")
 │
 ├─ BLE scan for "RMI-<serial>" → connect → negotiate MTU 512
 ├─ Subscribe to RX notifications
 │
 ├─ Read NVS for stored encRand
 │   ├─ NOT found → V0 pairing:
 │   │     CMD_APP_INFO_DATA_RES_DTO (AES-128-CBC, SN-derived key)
 │   │     Decrypt response → extract encRand → write to NVS
 │   └─ Found → skip pairing
 │
 ├─ CommCmd handshake (V1):
 │     action=64  login (includes generated bleId)
 │     action=104 time-sync
 │     Wait for status responses
 │
 └─ Poll loop every 30 s:
       Build CMD_REAL_RES_DTO V1 request
       Send, receive + reassemble paged response(s)
       Decrypt AES-128-GCM, verify auth tag
       Decode nanopb → scale fields → publish MQTT
       Publish status = "online" on first successful poll
```

**Paging:** RealDataNew responses may span multiple BLE notifications (`ap` = total pages, `cp` = current page). Each page is requested sequentially and merged before decoding.

**Night mode:** The inverter shuts BLE off at sunset. BLE scan retries every 60 s. On reconnect, `encRand` remains valid — only the CommCmd handshake is re-run, not V0 pairing.

## MQTT Topics

Base: `hoymiles/<inverter_serial>/`

### Top-level
```
status                 "online" / "offline"   (LWT = "offline")
energy_today           kWh                    (dtu_daily_energy ÷ 1000)
energy_total           kWh                    (sum of pv/*/energy_total)
firmware_version       string                 (published once on connect)
last_seen              Unix timestamp
```

### AC output (SGSMO)
```
ac/voltage             V      (raw ÷ 10)
ac/frequency           Hz     (raw ÷ 100)
ac/power               W      (raw ÷ 10)
ac/current             A      (raw ÷ 100)
ac/power_factor               (raw ÷ 100)
ac/temperature         °C     (raw ÷ 10)
ac/power_limit         W      (raw ÷ 10)
```

### Per-panel DC input (PvMO, port_number 1..N)
```
pv/1/voltage           V      (raw ÷ 10)
pv/1/current           A      (raw ÷ 100)
pv/1/power             W      (raw ÷ 10)
pv/1/energy_today      kWh    (raw ÷ 1000)
pv/1/energy_total      kWh    (raw ÷ 1000)
pv/2/...               (second panel)
```

**Note on scale factors:** derived from the hiflow-ble Python library output vs. raw protobuf `int32` values. Follow standard Hoymiles convention but must be validated against the real device on first run.

## Error Handling

| Situation | Behaviour |
|-----------|-----------|
| WiFi lost | `WiFi.reconnect()` with 10 s retry |
| MQTT unreachable | Retry connect every 10 s; discard data until reconnected |
| BLE disconnect (night / unexpected) | Scan retry every 60 s; re-run CommCmd on reconnect |
| V1 GCM auth tag failure | Log to serial, skip publish, retry next poll |
| Response timeout | Log to serial, skip publish, retry next poll |
| NVS read failure | Fall back to V0 pairing |
| Incomplete paged response | Discard partial result, retry full request next poll |

**Watchdog:** ESP32 task watchdog enabled at 30 s. Hung poll loop triggers clean reboot.

**Last Will and Testament:** registered at MQTT connect time. Broker publishes `status = "offline"` automatically if ESP32 loses connection.
