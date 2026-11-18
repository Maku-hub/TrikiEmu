#!/usr/bin/env python3
"""Jednorazowe komendy sterujace do emulatora TrikiEmu po USB-serial.

Protokol firmware (linie \\n):
  BLE,1 / BLE,0          wlacz/wylacz reklame BLE (parowanie)
  R                      powrot do zrodla domyslnego ruchu (IMU/spoczynek)
  M,gx,gy,gz,ax,ay,az    ustaw ruch (gyro deg/s, accel g)
  ADDR                   pokaz adres BLE (MAC)
  ADDR,XX:XX:XX:XX:XX:XX ustaw MAC kapsla (zapis w pamieci plytki + restart)

Uzycie:
    python tools/pc_control.py --port COM4 ble-on
    python tools/pc_control.py --port COM4 ble-off
    python tools/pc_control.py --port COM4 rest
    python tools/pc_control.py --port COM4 raw "M,0,0,90,0,0,1"
    python tools/pc_control.py show-mac
    python tools/pc_control.py set-mac                  # zapyta o adres
    python tools/pc_control.py set-mac EC:00:00:00:00:01
"""
import argparse
import time

from _serial_util import open_serial

ALIASES = {
    "ble-on":  "BLE,1",
    "ble-off": "BLE,0",
    "rest":    "R",
}


def read_reply(s, seconds, mark):
    """Czyta log plytki przez `seconds` s i wypisuje linie zawierajace `mark`."""
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        buf += s.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("ascii", "replace").strip()
            if mark in text:
                print(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="np. COM4 / /dev/ttyUSB0; domyslnie autodetekcja")
    ap.add_argument("cmd", help="ble-on | ble-off | rest | raw | show-mac | set-mac")
    ap.add_argument("arg", nargs="?", help="tresc dla 'raw' / adres dla 'set-mac'")
    args = ap.parse_args()

    reply_s = 0.0
    if args.cmd == "raw":
        if not args.arg:
            ap.error("'raw' wymaga tresci, np. raw \"M,0,0,90,0,0,1\"")
        line = args.arg
    elif args.cmd in ALIASES:
        line = ALIASES[args.cmd]
    elif args.cmd == "show-mac":
        line, reply_s = "ADDR", 0.5
    elif args.cmd == "set-mac":
        mac = args.arg or input("MAC kapsla (XX:XX:XX:XX:XX:XX): ")
        line, reply_s = "ADDR," + mac.strip().upper(), 3.0   # walidacja + restart po stronie plytki
    else:
        ap.error(f"nieznana komenda: {args.cmd}")

    s = open_serial(args.port, timeout=0.2)  # bez resetu; autodetekcja gdy None
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write((line + "\n").encode("ascii"))
    s.flush()
    if reply_s:
        read_reply(s, reply_s, "ADDR")
    else:
        time.sleep(0.2)
        print(f"Wyslano: {line}")
    s.close()


if __name__ == "__main__":
    main()
