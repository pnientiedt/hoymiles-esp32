#!/usr/bin/env python3
"""Ground-truth test: use the known-working hiflow-ble library from a host (Mac
tested) to pair with the inverter and extract encRand.

If this succeeds, the inverter is willing and any failure is in our ESP32
firmware (and we learn exactly what the working path does). If this ALSO fails,
the problem is environmental (inverter state / another client holding the BLE
slot).

Run with the ESP32 UNPLUGGED so it isn't holding the inverter's single BLE slot.

No device-specific values are baked in — pass the BLE advertisement name; the SN
is the 12-char tail after "RMI-".

Usage: ble-test/.venv/bin/python ble-test/pair_test.py <RMI-name>
       (or set HOYMILES_BLE_NAME in the environment)
"""
import asyncio
import logging
import os
import sys

from bleak import BleakScanner
from hiflow_ble.hiflow import HiFlow

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger("pair_test")


async def main():
    name = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("HOYMILES_BLE_NAME", "")
    if not name:
        print("usage: pair_test.py <RMI-name>   (e.g. RMI-XXXXXXXXXXXX)")
        return 2
    sn = name[len("RMI-"):] if name.startswith("RMI-") else name

    log.info("Scanning for %s ...", name)
    dev = await BleakScanner.find_device_by_name(name, timeout=20)
    if not dev:
        log.error("Device %s not found (ESP32 still holding the slot? out of range?)", name)
        return 1
    log.info("Found %s at %s", name, dev.address)

    hf = HiFlow(dev.address, sn=sn)
    try:
        log.info("Connecting (includes any BLE pair/bond the library performs) ...")
        await hf.connect()
        log.info("Connected. Running V0 pairing (extract encRand) ...")
        enc = await hf.async_extract_enc_rand()
        log.info("SUCCESS — encRand = %s", enc.hex())
        print("\nENC_RAND:", enc.hex())
        return 0
    except Exception as e:
        log.exception("Pairing FAILED: %s", e)
        return 2
    finally:
        try:
            await hf.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
