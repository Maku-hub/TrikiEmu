# Changelog

Wszystkie istotne zmiany w projekcie są dokumentowane w tym pliku.

Format wzorowany na [Keep a Changelog](https://keepachangelog.com/pl/1.1.0/),
projekt stosuje [Semantic Versioning](https://semver.org/lang/pl/).

## [1.0.0] - 2026-06-18

Pierwsze stabilne wydanie. Interoperacyjność z aplikacją Żappka **potwierdzona na
M5StickC Plus2** (kapsel rozpoznany, reakcja na ruch).

### Dodane

- **Firmware emulatora** (ESP32, Arduino + NimBLE): BLE *peripheral* udający kapsel
  Żabka Triki — reklama jak Triki, Nordic UART Service, strumień ramek IMU 14 B
  (~98 Hz), usługi Battery i Device Information, charakterystyka vendor (LED).
- **Odtworzenie tożsamości BLE**: adres MAC (random static, specyficzny per egzemplarz),
  16-bitowy service UUID w scan response, manufacturer data, Firmware Revision.
- **Przenośność**: rdzeń działa na dowolnym ESP32 (`env:esp32dev`); funkcje M5StickC
  Plus2 (ekran, przyciski, IMU, bateria, deep sleep) pod flagą `-D HAS_M5`
  (`env:m5stickc_plus2`).
- **Sterowanie ruchem z PC** (USB-serial): klawiatura na żywo (`pc_keyboard.py`),
  odtwarzanie sekwencji z CSV (`pc_motion_file.py`), generator wzorca
  (`pc_motion_feed.py`), odtwarzanie realnej nagranej sesji z btsnoop
  (`pc_replay_capture.py`), komendy jednorazowe (`pc_control.py`).
- **Narzędzia RE**: parser strumienia IMU z btsnoop (`parse_btsnoop.py`) oraz dumper
  całego ruchu ATT z mapowaniem handle→UUID (`btsnoop_att.py`).
- LICENSE (MIT), CHANGELOG, CI (GitHub Actions: build firmware + sprawdzenie narzędzi).

### Ustalenia z reverse-engineeringu

- Aplikacja rozpoznaje zapamiętany kapsel po **adresie MAC** — emulator musi go odtworzyć.
- **Zapis najlepszego wyniku w grach jest bramkowany kryptograficznym uwierzytelnieniem
  urządzenia** (challenge-response z sekretnym kluczem kapsla, dodatkowo walidacja
  serwerowa po stronie Żabki). Rozgrywka działa na surowym strumieniu IMU (emulator gra),
  ale **zapis wyniku wymaga prawdziwego kapsla** — jest poza zasięgiem emulatora i poza
  zakresem projektu (mechanizm anty-cheat). Szczegóły w `README.md`.

[1.0.0]: https://github.com/Maku-hub/TrikiEmu/releases/tag/v1.0.0
