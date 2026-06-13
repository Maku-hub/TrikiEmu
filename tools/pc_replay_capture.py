#!/usr/bin/env python3
"""Odtwarzanie REALNEJ nagranej sesji (btsnoop) jako ruch -> emulator TrikiEmu.

Wyciaga ramki IMU ze strumienia TX prawdziwego kapsla z capture'u HCI i wysyla je
jako linie "M,gx,gy,gz,ax,ay,az". To prawdziwy, wczesniej zarejestrowany ruch
(do testow/wiernosci), nie generowany wzorzec.

Reuzywa parsera: tools/parse_btsnoop.py.

Uzycie:
    python tools/pc_replay_capture.py                      # captures/btsnoop_hci.log
    python tools/pc_replay_capture.py --file inny.log --rate 100 --loop
"""
import argparse
import time

from _serial_util import open_serial
import parse_btsnoop as pb  # ten sam katalog (tools/)


def load_frames(path):
    chunks = []
    for _ts, _dir, data in pb.parse_btsnoop(path):
        att = pb.extract_att(data)
        if att and att[0] == 0x1b and att[1] == pb.TX_HANDLE:
            chunks.append(att[2])
    stream = b"".join(chunks)
    return pb.parse_frames(stream)  # lista dict: gx..gz [deg/s], ax..az [g]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="captures/btsnoop_hci.log")
    ap.add_argument("--port", default=None, help="np. COM4; domyslnie autodetekcja")
    ap.add_argument("--rate", type=float, default=100.0, help="Hz wysylki")
    ap.add_argument("--skip", type=int, default=20, help="pomin N pierwszych ramek (szum startu)")
    ap.add_argument("--loop", action="store_true")
    args = ap.parse_args()

    frames = load_frames(args.file)
    if args.skip > 0:
        frames = frames[args.skip:]
    if not frames:
        raise SystemExit(f"Brak ramek IMU w {args.file}")

    s = open_serial(args.port)  # bez resetu; autodetekcja gdy None
    period = 1.0 / args.rate
    print(f"Odtwarzam {len(frames)} realnych ramek z '{args.file}' @ {args.rate:.0f} Hz"
          f"{' (loop)' if args.loop else ''}. Ctrl+C konczy.")
    t0 = time.perf_counter()
    sent = 0
    try:
        while True:
            for fr in frames:
                s.write(f"M,{fr['gx']:.3f},{fr['gy']:.3f},{fr['gz']:.3f},"
                        f"{fr['ax']:.3f},{fr['ay']:.3f},{fr['az']:.3f}\n".encode("ascii"))
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
    print(f"Gotowe, wyslano {sent} ramek.")


if __name__ == "__main__":
    main()
