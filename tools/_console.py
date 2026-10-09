#!/usr/bin/env python3
"""Wspolne narzedzia konsoli dla interaktywnych skryptow PC TrikiEmu (Windows + Linux).

- enable_ansi(): wlacz sekwencje ANSI w konsoli Windows (na Linuksie nic nie robi).
- KeyReader: nieblokujacy odczyt klawiszy (Windows: msvcrt; Linux/macOS: termios + select).
  read() zwraca liste klawiszy: pojedyncze znaki ('q', ' ', ...) albo nazwy
  strzalek 'LEFT' / 'RIGHT' / 'UP' / 'DOWN'.
- get_cursor_x(): pozioma pozycja kursora myszy (Windows: GetCursorPos; Linux: X11
  XQueryPointer - dziala w sesji X11/XWayland, w czystym Wayland niedostepne).

Uruchomione bezposrednio pokazuje wcisniete klawisze i pozycje myszy (x = wyjscie):
    python tools/_console.py
"""
import ctypes
import ctypes.util
import os
import sys
import time

IS_WINDOWS = os.name == "nt"

if IS_WINDOWS:
    import msvcrt
else:
    import select
    import termios
    import tty


def enable_ansi():
    if not IS_WINDOWS:
        return
    try:
        k32 = ctypes.windll.kernel32
        k32.SetConsoleMode(k32.GetStdHandle(-11), 7)
    except Exception:
        pass


class KeyReader:
    """Kontekst: w srodku terminal w trybie cbreak (Linux), read() nie blokuje."""

    _WIN_ARROWS = {b"K": "LEFT", b"M": "RIGHT", b"H": "UP", b"P": "DOWN"}
    _ANSI_ARROWS = {"D": "LEFT", "C": "RIGHT", "A": "UP", "B": "DOWN"}

    def __enter__(self):
        if not IS_WINDOWS:
            if not sys.stdin.isatty():
                raise SystemExit("Wymaga prawdziwego terminala (stdin nie jest TTY).")
            self._fd = sys.stdin.fileno()
            self._old = termios.tcgetattr(self._fd)
            tty.setcbreak(self._fd)
        return self

    def __exit__(self, *exc):
        if not IS_WINDOWS:
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._old)
        return False

    def read(self):
        return self._read_windows() if IS_WINDOWS else self._read_posix()

    def _read_windows(self):
        keys = []
        while msvcrt.kbhit():
            ch = msvcrt.getch()
            if ch in (b"\x00", b"\xe0"):     # klawisze specjalne (strzalki)
                k2 = msvcrt.getch() if msvcrt.kbhit() else b""
                if k2 in self._WIN_ARROWS:
                    keys.append(self._WIN_ARROWS[k2])
                continue
            keys.append(ch.decode("ascii", "ignore"))
        return keys

    def _read_posix(self):
        data = b""
        while select.select([self._fd], [], [], 0)[0]:
            chunk = os.read(self._fd, 64)
            if not chunk:
                break
            data += chunk
        s = data.decode("ascii", "ignore")
        keys, i = [], 0
        while i < len(s):
            if s[i] == "\x1b" and s[i + 1:i + 2] in ("[", "O") and i + 2 < len(s):
                name = self._ANSI_ARROWS.get(s[i + 2])
                if name:
                    keys.append(name)
                i += 3
                continue
            keys.append(s[i])                    # Ctrl+C: cbreak zostawia ISIG -> SIGINT
            i += 1
        return keys


# ---- pozycja myszy ----

class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


_x11 = None


def _x11_init():
    global _x11
    if _x11 is not None:
        return _x11
    path = ctypes.util.find_library("X11")
    if not path or not os.environ.get("DISPLAY"):
        raise SystemExit("Mysz na Linuksie wymaga X11/XWayland (libX11 + DISPLAY). "
                         "W czystym Wayland uzyj --input keys.")
    lib = ctypes.cdll.LoadLibrary(path)
    lib.XOpenDisplay.restype = ctypes.c_void_p
    lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
    lib.XDefaultRootWindow.restype = ctypes.c_ulong
    lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    lib.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                  ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                                  ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                  ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                  ctypes.POINTER(ctypes.c_uint)]
    disp = lib.XOpenDisplay(None)
    if not disp:
        raise SystemExit("Nie udalo sie otworzyc ekranu X11. W Wayland uzyj --input keys.")
    _x11 = (lib, disp, lib.XDefaultRootWindow(disp))
    return _x11


def get_cursor_x():
    if IS_WINDOWS:
        p = _POINT()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(p))
        return p.x
    lib, disp, root = _x11_init()
    rr, cr = ctypes.c_ulong(), ctypes.c_ulong()
    rx, ry, wx, wy = (ctypes.c_int() for _ in range(4))
    mask = ctypes.c_uint()
    lib.XQueryPointer(disp, root, ctypes.byref(rr), ctypes.byref(cr),
                      ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(wx), ctypes.byref(wy),
                      ctypes.byref(mask))
    return rx.value


if __name__ == "__main__":
    enable_ansi()
    try:
        get_cursor_x()
        mouse = True
    except SystemExit as e:
        print(f"[mysz niedostepna] {e}")
        mouse = False
    print("Wciskaj klawisze (x = wyjscie)...")
    with KeyReader() as kr:
        while True:
            keys = kr.read()
            for k in keys:
                print(f"klawisz: {k!r}" + (f"   mysz x={get_cursor_x()}" if mouse else ""))
            if "x" in keys:
                break
            time.sleep(0.02)
