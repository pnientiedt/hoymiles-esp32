#!/usr/bin/env python3
"""Capture the ESP32 serial log to a timestamped file under logs/.

`pio device monitor` (miniterm) needs an interactive TTY and can't run headless,
so this uses pyserial directly. Each run writes logs/run_<YYYYmmdd_HHMMSS>.log
and tees the same output to stdout.

Usage:
    python3 tools/capture_serial.py [duration_seconds] [port]

Defaults: 120 seconds; port auto-detected from common ESP32 USB-UART names.
Pass --no-reset to attach without pulsing the board reset (keeps any in-progress
BLE session); by default it resets so the log captures a clean full run.
"""
import glob
import os
import sys
import time
from datetime import datetime

import serial  # pyserial

BAUD = 115200
PORT_GLOBS = [
    "/dev/cu.usbserial-*",
    "/dev/cu.SLAB_USBtoUART*",
    "/dev/cu.wchusbserial*",
    "/dev/cu.usbmodem*",
]


def autodetect_port():
    for pattern in PORT_GLOBS:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def sanitize(text):
    """Keep printable ASCII + tab/newline; drop the rest.

    The reset pulse makes the ESP32 boot ROM emit bytes at a different baud,
    which decode as binary garbage at 115200 and make the log unreadable. This
    strips those so each log file is clean text.
    """
    return "".join(c for c in text if c in "\t\n" or " " <= c <= "~")


def main():
    args = [a for a in sys.argv[1:] if a != "--no-reset"]
    do_reset = "--no-reset" not in sys.argv

    duration = float(args[0]) if len(args) > 0 else 120.0
    port = args[1] if len(args) > 1 else autodetect_port()
    if not port:
        sys.exit("No ESP32 serial port found. Plug in the board or pass the port "
                 "explicitly: python3 tools/capture_serial.py 120 /dev/cu.usbserial-XXXX")

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    logs_dir = os.path.join(repo_root, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = os.path.join(logs_dir, f"run_{stamp}.log")

    print(f"[capture] port={port} baud={BAUD} duration={duration:.0f}s reset={do_reset}")
    print(f"[capture] writing {log_path}")

    ser = serial.Serial(port, BAUD, timeout=1)
    if do_reset:
        # Pulse the reset line so the log starts at the boot banner.
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)

    end = time.time() + duration
    with open(log_path, "w") as f:
        header = f"=== capture start {datetime.now().isoformat()} port={port} ===\n"
        f.write(header)
        f.flush()
        while time.time() < end:
            line = ser.readline()
            if line:
                text = sanitize(line.decode("utf-8", "replace"))
                if text.strip("\n") == "":
                    continue  # skip lines that were pure boot-ROM garbage
                sys.stdout.write(text)
                sys.stdout.flush()
                f.write(text)
                f.flush()
    ser.close()
    print(f"\n[capture] done -> {log_path}")


if __name__ == "__main__":
    main()
