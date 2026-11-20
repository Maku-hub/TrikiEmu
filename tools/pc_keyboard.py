#!/usr/bin/env python3
"""Sterowanie ruchem emulatora TrikiEmu z KLAWIATURY (na zywo) po USB-serial.

Dwa tryby (przelacznik 't'):
  TRYB GIER (domyslny): q/e/a/d/w/s = obrot/przechyl (lewo/prawo), f = FLAP (FlappyBird).
  TRYB RZUTU (autotest podrzut+obroty): klawisze gier wylaczone; wpisujesz DWIE CYFRY
    (02-20) -> rzut nastepuje automatycznie. Ksztalt wzorowany na realnym capture.
    --throw-cal: 2 cyfry = czas wirowania x10 ms (kalibracja licznika gry).
  W obu trybach: p = przycisk kapsla (trzymasz p = przycisk wcisniety).

Pulpit (kilka linii) odswiezany w miejscu pokazuje na zywo wartosci i stan.

Dziala na Windows i Linuksie (wymaga prawdziwego terminala).
Uzycie:  python tools/pc_keyboard.py [--ble-on]
"""
import argparse
import math
import sys
import time

from _console import KeyReader, enable_ansi, key_is_down
from _serial_util import open_serial

RATE = 100.0
DT = 1.0 / RATE
DECAY = 0.85 ** (50.0 / RATE)
MAXANG = 80.0
STEP_MIN, STEP_MAX = 1.0, 120.0
FLAP_PEAK_MIN, FLAP_PEAK_MAX = 0.5, 30.0
FLAP_CYCLES = 1.5
MAX_THROW = 20

THROW_CYCLE_MS = 350.0
THROW_COUNT_A, THROW_COUNT_B = 3.29, -0.2   # licznik ~= 3.29*cykle - 0.2 -> cykle = (cel-B)/A
# Male liczby obrotow: wzor liniowy trafia w granice przedzialow (gra liczyla o 1 za duzo).
# Czasy wirowania [ms] ze srodkow przedzialow zmierzonych w Zappce (--throw-cal, 2026-11-20):
# <=190 ms -> 0 (rzut nie liczy sie), 210-230 -> 2, 234-300 -> 3, 340-420 -> 4,
# 447-520 -> 5, 580-660 -> 6, 700 -> 7. Gra nie pokazuje 1 obrotu (z 0 skacze na 2).
THROW_MS = {2: 220, 3: 280, 4: 380, 5: 495, 6: 620}
THROW_MIN = 2
BTN_MIN_S = 0.15   # najkrotsze wcisniecie przycisku kapsla (jak klikniecie B w firmware)
THROW_GYRO = -300.0
THROW_SPIN_AXIS = 0
AY_MID, AY_AMP = 1.5, 0.8
AZ_MID, AZ_AMP = -0.9, 0.15
WINDUP_MS, WINDUP_PEAK = 150.0, 5.0
LAND_MS, LAND_PEAK = 80.0, 10.0
NLINES = 4
# Kroki q/e/a/d/w/s, THROW_GYRO i przechyl dostrojono, gdy firmware kodowal gyro skala
# 131 LSB/(deg/s) zamiast 14.2857. Zeby gry zachowywaly sie tak samo, wewnetrzne
# predkosci zostaja w starych jednostkach, a do firmware'u idzie gyro x GYRO_GAIN.
# Przechyl (accel) dalej calkuje stare jednostki.
GYRO_GAIN = 131.0 / (1000.0 / 70.0)   # = 9.17


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="np. COM4 / /dev/ttyUSB0; domyslnie autodetekcja")
    ap.add_argument("--ble-on", action="store_true", help="wyslij BLE,1 na starcie")
    ap.add_argument("--step", type=float, default=15.0)
    ap.add_argument("--flap-peak", type=float, default=10.0)
    ap.add_argument("--flap-ms", type=float, default=120.0)
    ap.add_argument("--throw-cal", action="store_true",
                    help="kalibracja rzutu: 2 cyfry = czas wirowania x10 ms (np. 25 = 250 ms)")
    args = ap.parse_args()
    enable_ansi()

    s = open_serial(args.port, timeout=0.05)
    if args.ble_on:
        s.write(b"BLE,1\n"); s.flush()

    step = max(STEP_MIN, min(STEP_MAX, args.step))
    flap_peak = max(FLAP_PEAK_MIN, min(FLAP_PEAK_MAX, args.flap_peak))
    flap_ms = args.flap_ms
    wx = wy = wz = 0.0
    roll = pitch = 0.0
    flap_t = None
    throw_n = 0
    phase = None
    pt = 0.0
    throw_mode = False
    digit_buf = ""
    ble_state = "ON" if args.ble_on else "?"
    d2r = math.pi / 180.0

    # Przycisk kapsla. Gdy da sie czytac stan klawisza (Windows / X11): p wciska (B,1),
    # puszczenie p puszcza (B,0), min. BTN_MIN_S - przytrzymanie bez przerwy na autorepeat.
    # Inaczej (np. czysty Wayland): kazde zdarzenie p = klikniecie B (~150 ms w firmware).
    btn_real = key_is_down("p") is not None
    btn_down = False
    btn_t0 = 0.0
    btn_rel_t = 0.0
    btn_until = 0.0

    print("=== TrikiEmu klawiatura ===")
    rendered = [False]

    def render():
        if phase is not None:
            what = f"{throw_n * 10} ms" if args.throw_cal else f"~{throw_n} obr"
            l3 = f"  t = RZUT TRWA: {what} ({phase})"
        elif throw_mode and args.throw_cal:
            l3 = f"  t = KALIBRACJA RZUTU: 2 cyfry = czas x10 ms  [{digit_buf or '--'}]"
        elif throw_mode:
            l3 = f"  t = TRYB RZUTU: wpisz 2 cyfry {THROW_MIN:02d}-{MAX_THROW}  [{digit_buf or '--'}]"
        else:
            l3 = "  t = wejdz w TRYB RZUTU (autotest podrzut+obroty)"
        lines = [
            f"  q/e=lewo/prawo  w/s=obrot Y  a/d=obrot X  [ / ]=sila obrotu: {step:>3.0f}   gyro=({wx * GYRO_GAIN:+5.0f},{wy * GYRO_GAIN:+5.0f},{wz * GYRO_GAIN:+5.0f}) deg/s",
            f"  f=FLAP   , / .=sila flapa: {flap_peak:>2.0f} g   p=przycisk kapsla: "
            f"{'WCISNIETY' if btn_down or time.perf_counter() < btn_until else '-'}",
            l3,
            f"  b/n=BLE: {ble_state:<3}   spacja=zeruj   x=wyjscie",
        ]
        out = (f"\033[{NLINES}A" if rendered[0] else "")
        for ln in lines:
            out += "\r" + ln + "\033[K\n"
        sys.stdout.write(out); sys.stdout.flush()
        rendered[0] = True

    render()

    t0 = time.perf_counter(); n = 0
    with KeyReader() as kr:
        try:
            while True:
                changed = False
                for k in kr.read():
                    if len(k) != 1:      # strzalki itp. - nieuzywane w tym narzedziu
                        continue
                    k = k.lower()
                    changed = True
                    if k == "x":
                        raise KeyboardInterrupt
                    elif k == "p":           # przycisk kapsla (oba tryby)
                        if btn_real:
                            # spozniony autorepeat tuz po puszczeniu nie wciska ponownie
                            now_p = time.perf_counter()
                            if not btn_down and (key_is_down("p") or now_p - btn_rel_t > 0.1):
                                s.write(b"B,1\n"); btn_down = True; btn_t0 = now_p
                        else:
                            s.write(b"B\n"); btn_until = time.perf_counter() + BTN_MIN_S
                    elif k == "t":
                        throw_mode = not throw_mode; digit_buf = ""
                    elif k == "b": s.write(b"BLE,1\n"); ble_state = "ON"
                    elif k == "n": s.write(b"BLE,0\n"); ble_state = "OFF"
                    elif throw_mode:
                        if k.isdigit() and phase is None:
                            digit_buf += k
                            if len(digit_buf) >= 2:
                                if args.throw_cal:
                                    num = max(1, min(99, int(digit_buf)))
                                else:
                                    num = max(THROW_MIN, min(MAX_THROW, int(digit_buf)))
                                digit_buf = ""
                                throw_n = num; phase = "windup"; pt = 0.0
                    else:
                        if k == " ": wx = wy = wz = 0.0; roll = pitch = 0.0
                        elif k == "q": wz += step
                        elif k == "e": wz -= step
                        elif k == "a": wx -= step
                        elif k == "d": wx += step
                        elif k == "w": wy += step
                        elif k == "s": wy -= step
                        elif k == "f": flap_t = 0.0
                        elif k == "[": step = max(STEP_MIN, step - 1.0)
                        elif k == "]": step = min(STEP_MAX, step + 1.0)
                        elif k == ",": flap_peak = max(FLAP_PEAK_MIN, flap_peak - 1.0)
                        elif k == ".": flap_peak = min(FLAP_PEAK_MAX, flap_peak + 1.0)

                if (btn_down and time.perf_counter() - btn_t0 >= BTN_MIN_S
                        and not key_is_down("p")):
                    s.write(b"B,0\n"); btn_down = False; changed = True
                    btn_rel_t = time.perf_counter()

                roll = max(-MAXANG, min(MAXANG, roll + wx * DT))
                pitch = max(-MAXANG, min(MAXANG, pitch + wy * DT))
                ax = math.sin(pitch * d2r); ay = math.sin(roll * d2r)
                az = math.sqrt(max(0.0, 1.0 - ax * ax - ay * ay))
                gx, gy, gz = wx, wy, wz

                hold = False
                if phase is not None:
                    hold = True
                    pt += DT
                    if phase == "windup":
                        fr = pt / (WINDUP_MS / 1000.0)
                        ax = ay = 0.0; az = 1.0 + (WINDUP_PEAK - 1.0) * min(1.0, fr)
                        if pt >= WINDUP_MS / 1000.0: phase = "spin"; pt = 0.0
                    elif phase == "spin":
                        if args.throw_cal:
                            T = throw_n * 0.01
                            cycles = T / (THROW_CYCLE_MS / 1000.0)
                        elif throw_n in THROW_MS:
                            T = THROW_MS[throw_n] / 1000.0
                            cycles = T / (THROW_CYCLE_MS / 1000.0)
                        else:
                            cycles = (throw_n - THROW_COUNT_B) / THROW_COUNT_A
                            T = cycles * THROW_CYCLE_MS / 1000.0
                        ang = 2 * math.pi * cycles * (pt / T)
                        v = [0.0, 0.0, 0.0]; v[THROW_SPIN_AXIS] = THROW_GYRO
                        gx, gy, gz = v
                        ax = 0.0
                        ay = AY_MID + AY_AMP * math.cos(ang)
                        az = AZ_MID + AZ_AMP * math.sin(ang)
                        if pt >= T: phase = "land"; pt = 0.0
                    elif phase == "land":
                        ax = ay = 0.0; az = LAND_PEAK
                        if pt >= LAND_MS / 1000.0: phase = None

                if flap_t is not None and phase is None:
                    frac = flap_t / (flap_ms / 1000.0)
                    if frac >= 1.0: flap_t = None
                    else:
                        az += flap_peak * math.sin(math.pi * frac) * math.sin(2 * math.pi * FLAP_CYCLES * frac)
                        flap_t += DT
                elif flap_t is not None:
                    flap_t = None

                gx *= GYRO_GAIN; gy *= GYRO_GAIN; gz *= GYRO_GAIN
                s.write(f"M,{gx:.2f},{gy:.2f},{gz:.2f},{ax:.3f},{ay:.3f},{az:.3f}\n".encode("ascii"))

                if not hold:
                    wx *= DECAY; wy *= DECAY; wz *= DECAY

                n += 1
                if changed or n % 6 == 0:   # odswiez pulpit przy zmianie i ~16x/s (zywe gyro)
                    render()

                sleep = (t0 + n * DT) - time.perf_counter()
                if sleep > 0: time.sleep(sleep)
        except KeyboardInterrupt:
            sys.stdout.write("\nKoniec.\n")
        finally:
            s.write(b"R\n"); s.flush(); s.close()


if __name__ == "__main__":
    main()
