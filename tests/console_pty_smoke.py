#!/usr/bin/env python3
"""Smoke-test tools/_console.py na Linuksie (CI): KeyReader w pseudo-terminalu.

Proces potomny czyta klawisze przez KeyReader w pty, rodzic "wpisuje" znaki i
strzalki (sekwencje ANSI). Sprawdza dekodowanie i przywrocenie trybu terminala.
Tylko POSIX:  python tests/console_pty_smoke.py
"""
import os
import pty
import sys
import time

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
EXPECTED = ["q", "e", " ", "LEFT", "RIGHT", "x"]

pid, fd = pty.fork()
if pid == 0:
    sys.path.insert(0, TOOLS)
    import termios
    from _console import KeyReader
    before = termios.tcgetattr(0)
    got = []
    with KeyReader() as kr:
        t = time.time()
        while "x" not in got and time.time() - t < 5:
            got += kr.read()
            time.sleep(0.01)
    restored = termios.tcgetattr(0) == before
    print(f"RESULT {got!r} {restored}")
    sys.stdout.flush()
    os._exit(0)

time.sleep(0.5)
os.write(fd, b"qe \x1b[D\x1b[Cx")
out = b""
while True:
    try:
        chunk = os.read(fd, 1024)
    except OSError:
        break
    if not chunk:
        break
    out += chunk
os.waitpid(pid, 0)

line = next((ln for ln in out.decode(errors="replace").splitlines() if ln.startswith("RESULT")), "")
expected = f"RESULT {EXPECTED!r} True"
print(line or out)
if line != expected:
    raise SystemExit(f"FAIL: oczekiwano {expected}")
print("OK")
