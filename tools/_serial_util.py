#!/usr/bin/env python3
"""Wspolne narzedzia portu szeregowego dla skryptow PC TrikiEmu.

- autodetect_port(): znajdz port ESP32 po VID:PID typowych mostkow USB-serial.
- open_serial(port): otworz port BEZ resetu plytki (DTR/RTS = false).

Uruchomione bezposrednio wypisuje wykryte porty:
    python tools/_serial_util.py
"""
import serial
from serial.tools import list_ports

# VID-y typowych mostkow USB-serial na plytkach ESP32:
#   0x1A86 = WCH (CH340 0x7523 / CH9102 0x55D4 — m.in. M5StickC Plus2)
#   0x10C4 = Silicon Labs CP210x (wiele DevKitow ESP32)
#   0x0403 = FTDI
#   0x303A = Espressif (natywne USB w ESP32-S2/S3/C3)
KNOWN_VIDS = {0x1A86, 0x10C4, 0x0403, 0x303A}


def _candidates():
    known, other = [], []
    for p in list_ports.comports():
        (known if (p.vid in KNOWN_VIDS) else other).append(p)
    return known, other


def autodetect_port():
    """Zwraca nazwe portu (np. 'COM4') lub rzuca RuntimeError z podpowiedzia."""
    known, other = _candidates()
    if len(known) == 1:
        return known[0].device
    if len(known) > 1:
        lst = ", ".join(f"{p.device} ({p.description})" for p in known)
        raise RuntimeError(f"Kilka pasujacych portow ESP32: {lst}. Podaj --port.")
    # brak znanych VID-ow
    allp = known + other
    if len(allp) == 1:
        return allp[0].device
    if not allp:
        raise RuntimeError("Nie znaleziono zadnego portu szeregowego. Podlacz plytke.")
    lst = ", ".join(f"{p.device} ({p.description})" for p in allp)
    raise RuntimeError(f"Nie rozpoznano portu ESP32. Dostepne: {lst}. Podaj --port.")


def open_serial(port=None, baudrate=115200, timeout=0.1):
    """Otwiera port bez resetowania plytki. Gdy port=None -> autodetekcja."""
    if port is None:
        port = autodetect_port()
        print(f"[auto] port: {port}")
    s = serial.Serial()
    s.port = port
    s.baudrate = baudrate
    s.timeout = timeout
    s.dtr = False
    s.rts = False
    s.open()
    s.setDTR(False)
    s.setRTS(False)
    return s


if __name__ == "__main__":
    known, other = _candidates()
    print("Porty z known VID (ESP32):")
    for p in known:
        print(f"  {p.device}  VID:PID={p.vid:04X}:{p.pid:04X}  {p.description}")
    print("Pozostale porty:")
    for p in other:
        print(f"  {p.device}  {p.description}")
    try:
        print("Autodetekcja ->", autodetect_port())
    except RuntimeError as e:
        print("Autodetekcja: ", e)
