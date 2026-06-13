#!/usr/bin/env python3
"""Generator ruchu z PC -> M5Stick (emulator Triki) po USB-serial.

Wysyla linie "M,gx,gy,gz,ax,ay,az\\n" (gyro deg/s, accel g), ktore firmware
pakuje w ramki Triki i nadaje po BLE. Sluzy do WALIDACJI sciezki
PC -> M5Stick -> central (TrikiScope) deterministycznym wzorcem testowym.

Wzorzec domyslny: powolny obrot sinusoidalny wokol osi Z (gyro Z oscyluje),
accel trzyma ~1 g z lekkim przechylem. Latwo rozpoznac w dekoderze.

Uzycie:
    python tools/pc_motion_feed.py --port COM4 [--seconds 12] [--pattern spin|tilt|rest]
"""
import argparse
import math
import time

from _serial_util import open_serial


def sample(pattern: str, t: float):
    """Zwraca (gx,gy,gz, ax,ay,az) dla chwili t [s]."""
    if pattern == "rest":
        return (0.0, 0.0, 0.0, 0.0, 0.0, 1.0)
    if pattern == "tilt":
        # powolne przechylanie: accel przenosi sie miedzy osiami, gyro male
        roll = math.sin(2 * math.pi * 0.25 * t)   # 0.25 Hz
        return (0.0, 0.0, 0.0, math.sin(roll), 0.0, math.cos(roll))
    # "spin": obrot wokol Z, gyroZ oscyluje +-200 deg/s @ 0.5 Hz
    gz = 200.0 * math.sin(2 * math.pi * 0.5 * t)
    gx = 30.0 * math.sin(2 * math.pi * 0.5 * t + 1.0)
    return (gx, 0.0, gz, 0.0, 0.0, 1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="np. COM4; domyslnie autodetekcja")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--rate", type=float, default=50.0, help="Hz wysylki")
    ap.add_argument("--pattern", default="spin", choices=["spin", "tilt", "rest"])
    args = ap.parse_args()

    s = open_serial(args.port)  # bez resetu; autodetekcja gdy None

    print(f"Wysylam wzorzec '{args.pattern}' na {args.port} przez {args.seconds:.0f}s "
          f"@ {args.rate:.0f} Hz...")
    t0 = time.perf_counter()
    period = 1.0 / args.rate
    n = 0
    try:
        while True:
            t = time.perf_counter() - t0
            if t >= args.seconds:
                break
            gx, gy, gz, ax, ay, az = sample(args.pattern, t)
            line = f"M,{gx:.3f},{gy:.3f},{gz:.3f},{ax:.3f},{ay:.3f},{az:.3f}\n"
            s.write(line.encode("ascii"))
            n += 1
            # rownomierne tempo
            nxt = t0 + n * period
            sleep = nxt - time.perf_counter()
            if sleep > 0:
                time.sleep(sleep)
    finally:
        s.write(b"R\n")  # powrot do realnego IMU
        s.flush()
        s.close()
    print(f"Gotowe, wyslano {n} probek.")


if __name__ == "__main__":
    main()
