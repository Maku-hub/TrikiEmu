#!/usr/bin/env python3
"""Wygodne sterowanie gry 2 (waz z kulek) emulatorem TrikiEmu - OBROT (gyro Z).

Gra 2 steruje OBROTEM kapsla (gyro/yaw), nie statycznym przechylem - tak jak
klawisze q/e w pc_keyboard.py (ustawialy gz). Wysylamy gz [deg/s], accel plasko.
Gra NASYCA skret ~30 deg/s (powyzej bez roznicy) - stad maly domyslny max-rate.

Model wejscia:
  --input mouse (domyslne): gz ~ PREDKOSC ruchu myszy w poziomie. Ruszasz mysz
        w lewo -> waz skreca w lewo; PRZESTAJESZ ruszac (lub ruch minimalny)
        -> gz=0 -> waz jedzie PROSTO. Nie trzeba wracac kursorem do srodka.
  --input keys: a/strzalka-lewo, d/strzalka-prawo (skret gdy trzymasz, puszczasz -> prosto).

Gz dochodzi plynnie (low-pass). Strona zalezna od gry - jesli lustrzane, dodaj --invert.
Klawisze zawsze: spacja=wyzeruj, b/n=BLE on/off, x=wyjscie.
ZAKRES: tryb nierankingowy (zob. README).

Uzycie:
    python tools/pc_game2_drive.py                       # mysz, autodetekcja portu
    python tools/pc_game2_drive.py --input keys
"""
import argparse
import ctypes
import math
import sys
import time

from _serial_util import open_serial

try:
    import msvcrt
except ImportError:
    msvcrt = None

RATE = 100.0
DT = 1.0 / RATE
NLINES = 4


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def get_cursor_x():
    p = _POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(p))
    return p.x


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--input", default="mouse", choices=["mouse", "keys"])
    ap.add_argument("--max-rate", type=float, default=30.0, help="maks. predkosc obrotu [deg/s]; gra nasyca ~30")
    ap.add_argument("--mouse-gain", type=float, default=0.015, help="deg/s na (px/s) ruchu myszy; mniej = delikatniej")
    ap.add_argument("--mouse-deadzone", type=float, default=40.0, help="px/s ponizej = brak skretu (anty-drzenie)")
    ap.add_argument("--smooth-ms", type=float, default=60.0, help="stala czasowa dochodzenia gz")
    ap.add_argument("--release-ms", type=float, default=140.0, help="keys: powrot do prosto po puszczeniu")
    ap.add_argument("--invert", action="store_true", help="odwroc strone sterowania")
    ap.add_argument("--ble-on", action="store_true")
    args = ap.parse_args()
    if msvcrt is None:
        raise SystemExit("Wymaga Windows (msvcrt).")

    try:
        ctypes.windll.kernel32.SetConsoleMode(ctypes.windll.kernel32.GetStdHandle(-11), 7)
    except Exception:
        pass

    s = open_serial(args.port)
    if args.ble_on:
        s.write(b"BLE,1\n"); s.flush()

    sign = -1.0 if args.invert else +1.0       # domyslnie: mysz w lewo -> waz w lewo
    last_x = get_cursor_x()
    vel = 0.0
    actual_rate = 0.0                          # gz [deg/s], dochodzi plynnie
    key_dir = 0.0
    last_key_t = -1.0
    ble_state = "ON" if args.ble_on else "?"
    alpha = 1.0 - math.exp(-DT / (args.smooth_ms / 1000.0))

    rendered = [False]

    def render(target):
        side = "LEWO" if actual_rate > 3 else "PRAWO" if actual_rate < -3 else "prosto"
        src = f"vel={vel:+7.0f}px/s" if args.input == "mouse" else f"keys={key_dir:+.0f}"
        lines = [
            f"  WEJSCIE: {args.input}   {'rusz mysz=skret, stop=prosto' if args.input=='mouse' else 'a/d lub strzalki'}"
            f"   invert={'T' if args.invert else 'N'}",
            f"  {src}  cel_gz={target:+6.1f}  akt_gz={actual_rate:+6.1f} deg/s  ({side})",
            f"  max={args.max_rate:.0f}deg/s  gain={args.mouse_gain:.3f}  dz={args.mouse_deadzone:.0f}px/s"
            f"  smooth={args.smooth_ms:.0f}ms",
            f"  b/n=BLE:{ble_state:<3}  spacja=zeruj  x=wyjscie",
        ]
        out = (f"\033[{NLINES}A" if rendered[0] else "")
        for ln in lines:
            out += "\r" + ln + "\033[K\n"
        sys.stdout.write(out); sys.stdout.flush()
        rendered[0] = True

    print("=== TrikiEmu: sterowanie gry 2 (obrot/gyro, model ruchu myszy) ===")
    t0 = time.perf_counter(); n = 0
    try:
        while True:
            now = time.perf_counter() - t0
            while msvcrt.kbhit():
                ch = msvcrt.getch()
                if ch in (b"\x00", b"\xe0"):     # klawisze specjalne (strzalki)
                    k2 = msvcrt.getch() if msvcrt.kbhit() else b""
                    if k2 == b"K": key_dir = -1.0; last_key_t = now   # lewo
                    elif k2 == b"M": key_dir = +1.0; last_key_t = now  # prawo
                    continue
                k = ch.decode("ascii", "ignore").lower()
                if k == "x": raise KeyboardInterrupt
                elif k == " ": key_dir = 0.0; actual_rate = 0.0
                elif k == "b": s.write(b"BLE,1\n"); ble_state = "ON"
                elif k == "n": s.write(b"BLE,0\n"); ble_state = "OFF"
                elif args.input == "keys":
                    if k == "a": key_dir = -1.0; last_key_t = now
                    elif k == "d": key_dir = +1.0; last_key_t = now

            if args.input == "mouse":
                cur = get_cursor_x()
                vel = (cur - last_x) / DT                    # px/s (lewo = ujemne)
                last_x = cur
                v = 0.0 if abs(vel) < args.mouse_deadzone else vel
                target = clamp(sign * (-v) * args.mouse_gain, -args.max_rate, args.max_rate)
            else:
                if now - last_key_t > args.release_ms / 1000.0:
                    key_dir = 0.0
                target = sign * key_dir * args.max_rate

            actual_rate += (target - actual_rate) * alpha     # plynne dochodzenie

            # accel plasko, sterujemy gyro Z (yaw) - jak q/e w pc_keyboard
            s.write(f"M,0.00,0.00,{actual_rate:.2f},0.000,0.000,1.000\n".encode("ascii"))

            n += 1
            if n % 6 == 0:
                render(target)

            sleep = (t0 + n * DT) - time.perf_counter()
            if sleep > 0:
                time.sleep(sleep)
    except KeyboardInterrupt:
        sys.stdout.write("\nKoniec.\n")
    finally:
        s.write(b"R\n"); s.flush(); s.close()


if __name__ == "__main__":
    main()
