#!/usr/bin/env python3
"""Dumper CALEGO ruchu ATT z btsnoop (poza strumieniem IMU) - do badania protokolu.

Pokazuje chronologicznie wszystkie operacje ATT (write/read/notify/indicate/discovery)
z mapowaniem handle->UUID (z odpowiedzi rozpoznania GATT). Sluzy do znalezienia wymiany
przy KONCU GRY (np. zapis najlepszego wyniku): jaka komende apka pisze i co kapsel odpowiada.

Uzycie:
    python tools/btsnoop_att.py captures/log.log               # caly ruch poza IMU
    python tools/btsnoop_att.py captures/log.log --from 460 --to 480   # okno czasowe [s]
    python tools/btsnoop_att.py captures/log.log --imu         # dolacz tez notyfikacje IMU
"""
import argparse
import struct

import parse_btsnoop as pb

OPN = {0x01: "ERROR", 0x02: "MTU-req", 0x03: "MTU-resp", 0x04: "FindInfo-req",
       0x05: "FindInfo-resp", 0x08: "ReadByType-req", 0x09: "ReadByType-resp",
       0x0a: "Read-req", 0x0b: "Read-resp", 0x0c: "ReadBlob-req", 0x0d: "ReadBlob-resp",
       0x10: "ReadByGrp-req", 0x11: "ReadByGrp-resp", 0x12: "Write-REQ", 0x13: "Write-resp",
       0x52: "Write-CMD", 0x1b: "NOTIFY", 0x1d: "INDICATE", 0x16: "PrepWrite", 0x18: "Execute",
       0x0e: "MultiRead-req", 0x0f: "MultiRead-resp", 0x1e: "HandleConfirm", 0x53: "SignedWrite"}

IMU_TX_HANDLE = 0x000F


def uuid_str(b):
    if len(b) == 2:
        return f"0x{int.from_bytes(b, 'little'):04x}"
    if len(b) == 16:
        return b[::-1].hex()
    return b.hex()


def build_handle_map(recs):
    """Z odpowiedzi ReadByType (char decl 0x2803) zbuduj val_handle -> UUID."""
    hmap = {}
    for ts, d, data in recs:
        att = pb.extract_att(data)
        if not att:
            continue
        op, _h, v = att
        if op == 0x09 and len(v) >= 2 and v[0] > 0:        # ReadByType-resp: [len][handle..]
            ln = v[0]
            for off in range(1, len(v) - ln + 1, ln):
                item = v[off:off + ln]
                if ln in (7, 21):             # char decl: props(1)+valhandle(2)+uuid(2/16)
                    val_h = struct.unpack_from("<H", item, 1)[0]
                    hmap[val_h] = uuid_str(item[3:])
    return hmap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--from", dest="t_from", type=float, default=0.0)
    ap.add_argument("--to", dest="t_to", type=float, default=1e9)
    ap.add_argument("--imu", action="store_true", help="dolacz notyfikacje IMU (handle 0x000f)")
    args = ap.parse_args()

    recs = list(pb.parse_btsnoop(args.path))
    if not recs:
        print("Pusty log."); return
    t0 = recs[0][0]
    hmap = build_handle_map(recs)
    print("Mapa handle->UUID (z rozpoznania GATT):")
    for h in sorted(hmap):
        print(f"  0x{h:04x} -> {hmap[h]}")
    print("=" * 78)

    nimu = 0
    for ts, d, data in recs:
        att = pb.extract_att(data)
        if not att:
            continue
        op, h, v = att
        if op == 0x1b and h == IMU_TX_HANDLE and not args.imu:
            nimu += 1
            continue
        rel = (ts - t0) / 1e6
        if rel < args.t_from or rel > args.t_to:
            continue
        nm = OPN.get(op, f"op0x{op:02x}")
        uu = f" ({hmap[h]})" if h in hmap else ""
        vs = v.hex(" ")
        if len(vs) > 96:
            vs = vs[:96] + "..."
        ascii_ = "".join(chr(c) if 32 <= c < 127 else "." for c in v[:24])
        print(f"t={rel:7.1f}s {d} {nm:13} h=0x{h:04x}{uu} [{len(v):2}] {vs}   |{ascii_}|")
    if not args.imu:
        print(f"\n(pominieto {nimu} notyfikacji IMU; --imu by je pokazac)")


if __name__ == "__main__":
    main()
