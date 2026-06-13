#!/usr/bin/env python3
"""Jednorazowe komendy sterujace do emulatora TrikiEmu po USB-serial.

Protokol firmware (linie \\n):
  BLE,1 / BLE,0          wlacz/wylacz reklame BLE (parowanie)
  R                      powrot do zrodla domyslnego ruchu (IMU/spoczynek)
  M,gx,gy,gz,ax,ay,az    ustaw ruch (gyro deg/s, accel g)

Uzycie:
    python tools/pc_control.py --port COM4 ble-on
    python tools/pc_control.py --port COM4 ble-off
    python tools/pc_control.py --port COM4 rest
    python tools/pc_control.py --port COM4 raw "M,0,0,90,0,0,1"
"""
import argparse
import time

from _serial_util import open_serial

ALIASES = {
    "ble-on":  "BLE,1",
    "ble-off": "BLE,0",
    "rest":    "R",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="np. COM4; domyslnie autodetekcja")
    ap.add_argument("cmd", help="ble-on | ble-off | rest | raw")
    ap.add_argument("arg", nargs="?", help="tresc dla 'raw'")
    args = ap.parse_args()

    if args.cmd == "raw":
        if not args.arg:
            ap.error("'raw' wymaga tresci, np. raw \"M,0,0,90,0,0,1\"")
        line = args.arg
    elif args.cmd in ALIASES:
        line = ALIASES[args.cmd]
    else:
        ap.error(f"nieznana komenda: {args.cmd}")

    s = open_serial(args.port, timeout=0.2)  # bez resetu; autodetekcja gdy None
    time.sleep(0.2)
    s.write((line + "\n").encode("ascii"))
    s.flush()
    time.sleep(0.2)
    s.close()
    print(f"Wyslano: {line}")


if __name__ == "__main__":
    main()
