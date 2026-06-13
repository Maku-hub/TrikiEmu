#!/usr/bin/env python3
"""Odtwarzanie ruchu z pliku CSV -> emulator TrikiEmu po USB-serial.

Wysyla linie "M,gx,gy,gz,ax,ay,az\\n" (gyro deg/s, accel g) wg sekwencji z pliku.
Na koniec wysyla "R" (powrot do zrodla domyslnego).

Format pliku (CSV, '#' = komentarz, biale znaki ignorowane):
  - 7 kolumn: t,gx,gy,gz,ax,ay,az  -> KEYFRAME'y; tool interpoluje liniowo miedzy
    nimi i wysyla z czestotliwoscia --rate az do ostatniego t. Krotki plik = plynny ruch.
  - 6 kolumn: gx,gy,gz,ax,ay,az    -> kazdy wiersz to jedna probka, grana przy --rate Hz.

Uzycie:
    python tools/pc_motion_file.py --port COM4 tools/motion_examples/demo.csv
    python tools/pc_motion_file.py --port COM4 ruch.csv --rate 100 --loop
"""
import argparse
import time

from _serial_util import open_serial


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(",")]
            rows.append([float(x) for x in parts])
    if not rows:
        raise ValueError("Plik nie zawiera danych")
    ncol = len(rows[0])
    if ncol not in (6, 7):
        raise ValueError(f"Oczekiwano 6 lub 7 kolumn, jest {ncol}")
    return rows, ncol


def frames_from_keyframes(rows, rate):
    """Generuje (gx..az) probki interpolujac keyframe'y [t,gx..az] przy 'rate' Hz."""
    rows = sorted(rows, key=lambda r: r[0])
    t_end = rows[-1][0]
    n = max(1, int(round(t_end * rate)))
    dt = 1.0 / rate
    j = 0
    for i in range(n + 1):
        t = i * dt
        while j + 1 < len(rows) and rows[j + 1][0] <= t:
            j += 1
        a = rows[j]
        b = rows[min(j + 1, len(rows) - 1)]
        span = b[0] - a[0]
        f = 0.0 if span <= 0 else max(0.0, min(1.0, (t - a[0]) / span))
        yield [a[1 + k] + (b[1 + k] - a[1 + k]) * f for k in range(6)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--port", default=None, help="np. COM4; domyslnie autodetekcja")
    ap.add_argument("--rate", type=float, default=50.0, help="Hz wysylki")
    ap.add_argument("--loop", action="store_true")
    args = ap.parse_args()

    rows, ncol = load(args.file)
    if ncol == 7:
        samples = list(frames_from_keyframes(rows, args.rate))
    else:
        samples = rows  # 6 kolumn = gotowe probki

    s = open_serial(args.port)  # bez resetu; autodetekcja gdy None

    period = 1.0 / args.rate
    print(f"Odtwarzam {len(samples)} probek z '{args.file}' @ {args.rate:.0f} Hz"
          f"{' (loop)' if args.loop else ''} na {args.port}. Ctrl+C konczy.")
    t0 = time.perf_counter()
    sent = 0
    try:
        while True:
            for smp in samples:
                gx, gy, gz, ax, ay, az = smp
                s.write(f"M,{gx:.3f},{gy:.3f},{gz:.3f},{ax:.3f},{ay:.3f},{az:.3f}\n"
                        .encode("ascii"))
                sent += 1
                nxt = t0 + sent * period
                sleep = nxt - time.perf_counter()
                if sleep > 0:
                    time.sleep(sleep)
            if not args.loop:
                break
    except KeyboardInterrupt:
        print("\nPrzerwano.")
    finally:
        s.write(b"R\n"); s.flush(); s.close()
    print(f"Gotowe, wyslano {sent} probek.")


if __name__ == "__main__":
    main()
