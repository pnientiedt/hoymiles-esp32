# ble-test — host-side BLE reference harness

Small manual harness that drives the inverter from a host (macOS/Linux) using the
known-working [`hiflow-ble`](https://github.com/TheTiEr/hiflow-ble) reference
library over CoreBluetooth/BlueZ. It exists to get **ground truth** independent of
the ESP32 firmware: if the reference can pair/authenticate but the firmware can't,
the bug is ours.

These scripts are a developer aid, not part of the firmware build.

## Setup

```bash
python3 -m venv ble-test/.venv
ble-test/.venv/bin/pip install -r ble-test/requirements.txt
```

The `.venv/` is gitignored.

## Usage

Run with the **ESP32 unplugged** — the inverter accepts only one BLE client at a
time, and the ESP32 otherwise holds the slot (the inverter stops advertising
while connected).

```bash
# V0 pairing only — extract encRand
ble-test/.venv/bin/python ble-test/pair_test.py RMI-XXXXXXXXXXXX

# Full handshake: validate a BLE PIN and whitelist a bleId, then read data.
# Pass the firmware's BLE_ID (src/config.h) so the PIN whitelists THAT identity.
ble-test/.venv/bin/python ble-test/pin_test.py <PIN> RMI-XXXXXXXXXXXX <bleId>
```

## Secrets

Nothing device-specific or secret is committed here. The BLE PIN and device
name/serial are passed as CLI args (or `HOYMILES_PIN` / `HOYMILES_BLE_NAME` /
`HOYMILES_BLE_ID` env vars) at run time — never hardcode them in these files.

> Note: repeated **wrong** PIN attempts lock the DTU for ~11 minutes. Validate a
> PIN here once rather than guessing on the ESP32.
