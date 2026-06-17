#!/usr/bin/env python3
"""Validate a BLE PIN against the inverter using the known-working hiflow-ble
reference, BEFORE risking the ESP32 (wrong PINs lock the DTU ~11 min).

Pass the SAME bleId the ESP32 firmware uses (BLE_ID in src/config.h) so a
successful PIN whitelists THAT bleId on the DTU — after which the ESP32 logs in
without a PIN.

Run with the ESP32 UNPLUGGED (it otherwise holds the inverter's single BLE slot).

Nothing device-specific is baked in. The PIN is only ever taken from the command
line / environment — never store it in this file.

Usage:
    ble-test/.venv/bin/python ble-test/pin_test.py <PIN> <RMI-name> [bleId]

Or via environment:
    HOYMILES_PIN=... HOYMILES_BLE_NAME=RMI-... HOYMILES_BLE_ID=... \
        ble-test/.venv/bin/python ble-test/pin_test.py
"""
import asyncio
import logging
import os
import sys

from bleak import BleakScanner
from hiflow_ble.hiflow import HiFlow, generate_ble_id

logging.basicConfig(level=logging.DEBUG,
                    format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger("pin_test")


async def main():
    pin    = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("HOYMILES_PIN", "")
    name   = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("HOYMILES_BLE_NAME", "")
    ble_id = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("HOYMILES_BLE_ID", "")
    if not name:
        print("usage: pin_test.py <PIN> <RMI-name> [bleId]   (PIN may be empty for none)")
        return 2
    sn = name[len("RMI-"):] if name.startswith("RMI-") else name
    if not ble_id:
        ble_id = generate_ble_id()
        log.info("No bleId given — generated %s (won't match the ESP32's BLE_ID)", ble_id)

    log.info("Scanning for %s ...", name)
    dev = await BleakScanner.find_device_by_name(name, timeout=20)
    if not dev:
        log.error("Device %s not found — ESP32 still holding the BLE slot, or out of range?", name)
        return 1
    log.info("Found %s at %s", name, dev.address)

    hf = HiFlow(dev.address, sn=sn, ble_id=ble_id, pin=pin)
    try:
        await hf.connect()
        log.info("Connected. Extracting encRand (V0) ...")
        enc = await hf.async_extract_enc_rand()
        log.info("encRand = %s", enc.hex())

        log.info("Running CommCmd handshake with bleId=%s ...", ble_id)
        ok = await hf.async_do_comm_cmd_handshake(ble_id=ble_id, pin=pin)
        log.info("handshake returned %s (ble_id now %s)", ok, hf.ble_id)

        log.info("Trying RealDataNew to confirm auth ...")
        data = await hf.async_get_real_data_new()
        if data is not None:
            print("\n=== SUCCESS: data flows ===")
            print(data)
            return 0
        print("\n=== handshake ran but RealDataNew returned None ===")
        print("(inverter may be dark/offline at night — check the sts= lines above)")
        return 0
    except Exception as e:
        log.exception("FAILED: %s", e)
        return 2
    finally:
        try:
            await hf.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
