#!/usr/bin/env python3
"""Parser btsnoop_hci.log -> wyciaga sesje Triki (NUS).

Robi to, czego tshark nie pokazuje wprost (handle NUS niezmapowane do UUID):
- zapisy telefon->RX (handle 0x000d): komendy start/stop,
- notyfikacje TX (handle 0x000f): sklejone w ciagly strumien i pociete na
  ramki 14 B IMU (naglowek 0x22, status, 6x int16 LE: gyro XYZ, accel XYZ).

Skale sprzetowe (LSM6DSL): gyro 131.0 LSB/(deg/s), accel 2048.0 LSB/g.
Handle (z tej sesji): RX=0x000d, TX=0x000f, CCCD TX=0x0010.

Uzycie:
    python tools/parse_btsnoop.py captures/btsnoop_hci.log
"""
import struct
import sys

# --- handle z tej konkretnej sesji (potwierdzone tsharkiem) ---
RX_HANDLE = 0x000D     # telefon -> kapsel (komendy)
TX_HANDLE = 0x000F     # kapsel -> telefon (strumien IMU)

GYRO_SCALE = 131.0     # LSB/(deg/s)
ACCEL_SCALE = 2048.0   # LSB/g

# btsnoop epoch: mikrosekundy od 0000-01-01; offset do epoki Unix.
BTSNOOP_EPOCH_DELTA_US = 0x00dcddb30f2f8000


def parse_btsnoop(path):
    """Yield (ts_us_unix, direction, h4_payload) dla kazdego rekordu."""
    with open(path, "rb") as f:
        hdr = f.read(16)
        if hdr[:8] != b"btsnoop\x00":
            raise ValueError("To nie jest plik btsnoop")
        while True:
            rec = f.read(24)
            if len(rec) < 24:
                break
            orig_len, incl_len, flags, drops, ts = struct.unpack(">IIIIq", rec)
            data = f.read(incl_len)
            if len(data) < incl_len:
                break
            ts_unix_us = ts - BTSNOOP_EPOCH_DELTA_US
            direction = "rx" if (flags & 0x01) else "tx"  # 1=controller->host
            yield ts_unix_us, direction, data


def extract_att(data):
    """Z H4 ACL wyciaga (att_opcode, handle, value) lub None."""
    if not data or data[0] != 0x02:  # tylko ACL
        return None
    if len(data) < 9:
        return None
    # [0]=H4 typ, [1:3]=handle+flags, [3:5]=acl len, [5:7]=l2cap len, [7:9]=cid
    l2cap_len, cid = struct.unpack_from("<HH", data, 5)
    if cid != 0x0004:  # tylko ATT
        return None
    att = data[9:9 + l2cap_len]
    if len(att) < 3:
        return None
    opcode = att[0]
    handle = struct.unpack_from("<H", att, 1)[0]
    value = att[3:]
    return opcode, handle, value


def main(path):
    writes = []           # (ts, value) -> RX
    tx_chunks = []        # (ts, value) <- TX
    for ts, _dir, data in parse_btsnoop(path):
        att = extract_att(data)
        if att is None:
            continue
        opcode, handle, value = att
        if opcode in (0x52, 0x12) and handle == RX_HANDLE:      # write cmd/req
            writes.append((ts, value))
        elif opcode == 0x1b and handle == TX_HANDLE:            # notification
            tx_chunks.append((ts, value))

    print("=" * 60)
    print(f"Zapisy telefon->RX (0x{RX_HANDLE:04x}): {len(writes)}")
    print("=" * 60)
    seen = {}
    for ts, v in writes:
        seen.setdefault(v.hex(" "), 0)
        seen[v.hex(" ")] += 1
    for hexv, n in seen.items():
        kind = "START" if hexv.startswith("20 10") else (
            "STOP" if hexv.startswith("20 00") else "?")
        print(f"  [{kind:5}] x{n:<3} {hexv}")

    print()
    print("=" * 60)
    print(f"Notyfikacje TX (0x{TX_HANDLE:04x}): {len(tx_chunks)} kawalkow")
    print("=" * 60)
    sizes = {}
    for _ts, v in tx_chunks:
        sizes[len(v)] = sizes.get(len(v), 0) + 1
    print("  Rozmiary kawalkow:", dict(sorted(sizes.items())))

    # sklej ciagly strumien
    stream = b"".join(v for _ts, v in tx_chunks)
    if tx_chunks:
        dur = (tx_chunks[-1][0] - tx_chunks[0][0]) / 1e6
    else:
        dur = 0.0
    print(f"  Lacznie bajtow strumienia: {len(stream)}")
    print(f"  Czas trwania strumienia:   {dur:.1f} s")
    if dur > 0:
        print(f"  Przeplyw bajtow:           {len(stream)/dur:.0f} B/s")

    # potnij na ramki 14 B; wyrownaj do naglowka 0x22
    frames = parse_frames(stream)
    print()
    print("=" * 60)
    print(f"Ramki IMU 14 B: {len(frames)}")
    print("=" * 60)
    if frames:
        if dur > 0:
            print(f"  Kadencja ramek: {len(frames)/dur:.1f} Hz")
        buttons = sum(1 for fr in frames if fr["button"])
        print(f"  Ramki z wcisnietym przyciskiem (bit0 status): {buttons}")
        # spoczynkowa magnituda accel z pierwszych 50 (po pominieciu 20 szumu)
        import math
        sample = frames[20:70] if len(frames) > 70 else frames
        mags = [math.sqrt(fr["ax"]**2 + fr["ay"]**2 + fr["az"]**2)
                for fr in sample]
        if mags:
            print(f"  Srednia |accel| (probki 20-70): "
                  f"{sum(mags)/len(mags):.3f} g  (oczekiwane ~1.0)")
        # statusy jakie wystapily
        stats = sorted({fr["status"] for fr in frames})
        print(f"  Wartosci bajtu status: {[hex(s) for s in stats]}")
        print()
        print("  Pierwsze 5 zdekodowanych ramek (deg/s, g):")
        for fr in frames[:5]:
            print(f"    st=0x{fr['status']:02x} "
                  f"gyro=({fr['gx']:+7.1f},{fr['gy']:+7.1f},{fr['gz']:+7.1f}) "
                  f"accel=({fr['ax']:+6.3f},{fr['ay']:+6.3f},{fr['az']:+6.3f})")


def parse_frames(stream):
    """Wyrownaj do 0x22 i tnij na ramki 14 B."""
    frames = []
    i = 0
    n = len(stream)
    while i + 14 <= n:
        if stream[i] != 0x22:
            i += 1
            continue
        raw = stream[i:i + 14]
        status = raw[1]
        gx, gy, gz, ax, ay, az = struct.unpack_from("<6h", raw, 2)
        frames.append({
            "status": status,
            "button": bool(status & 0x01),
            "gx": gx / GYRO_SCALE, "gy": gy / GYRO_SCALE, "gz": gz / GYRO_SCALE,
            "ax": ax / ACCEL_SCALE, "ay": ay / ACCEL_SCALE, "az": az / ACCEL_SCALE,
        })
        i += 14
    return frames


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "captures/btsnoop_hci.log")
