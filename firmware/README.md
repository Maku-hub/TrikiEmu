# Firmware — emulator Triki na ESP32

BLE peripheral (GATT server) odtwarzający „kontrakt" prawdziwego kapsla Triki:
reklamuje się jak Triki, wystawia Nordic UART Service i po komendzie start nadaje
syntetyczny strumień IMU. Cała specyfikacja poniżej pochodzi z reverse-engineeringu
([TrikiScope](https://github.com/Maku-hub/TrikiScope) + własny przechwyt Android HCI snoop)
i jest potwierdzona na żywym sprzęcie (M5StickC Plus2 + aplikacja Żappka).

**Przenośny: rdzeń działa na dowolnym ESP32** (Arduino + `NimBLE-Arduino`).
M5StickC Plus2 (ekran/przyciski/IMU/bateria) to **opcja** włączana flagą `-D HAS_M5`.

- **Goły ESP32** (`env:esp32dev`): sterowanie wyłącznie z PC po USB-serial. Brak ekranu/przycisków.
- **M5StickC Plus2** (`env:m5stickc_plus2`): + ekran ze stanem, przyciski (BLE on/off, uśpienie),
  realny IMU MPU6886 jako alternatywne źródło ruchu, poziom baterii.

---

## Kontrakt protokołu (pełna specyfikacja)

To jest komplet tego, co emulator odtwarza, żeby Żappka uznała go za prawdziwy kapsel.
**Adres MAC musi pochodzić z prawdziwego kapsla** (Żappka łączy po MAC; przykładowy adres
nie działa — sprawdzone). Numer (Device ID) i nazwa nie są krytyczne. Ustawiasz je w
[src/main.cpp](src/main.cpp) (`BLE_ADDR_LE` — bajty LSB-first — oraz `DEVICE_NAME`).

### Reklama (advertising)

| Element | Wartość (przykład) | Uwaga |
|---|---|---|
| **Adres BLE** | `EC:07:A9:B8:C1:B7`, typ **random static** | **najważniejsze** — Żappka łączy po MAC; inny adres → „nie znaleziono triki" |
| Flags | `0x06` (LE General Disc + BR/EDR not supp.) | AD type `0x01` |
| Local name | `Triki 1234567890` | AD type `0x09`; sufiks = Device ID (przykładowy, niekrytyczny) |
| Manufacturer data | company `0xFF00`, payload `a0 0a` | AD type `0xFF` |
| **16-bit Service UUID** | `0x0001` | AD type `0x03`, w **scan response** — po tym appka filtruje skan |

Układ jak w kapslu: ADV = flags + mfr + name; SCAN_RSP = service UUID `0x0001`.
**Uwaga:** kapsel **nie** reklamuje 128-bitowego UUID NUS — nie dodawaj go do reklamy.

Adres MAC jest **specyficzny dla egzemplarza** — żeby użyć własnego kapsla, odczytaj jego
adres dowolnym skanerem BLE ([TrikiScope](https://github.com/Maku-hub/TrikiScope), nRF Connect)
— adres, nazwa, manufacturer data i service UUID są wprost w reklamie — i wpisz adres w
`BLE_ADDR_LE` (bajty LSB-first). Typ adresu i resztę potwierdza Android HCI snoop.

### Profil GATT

MTU negocjowane do **247**.

| Usługa | Charakterystyka | UUID | Właściwości | Wartość |
|---|---|---|---|---|
| Generic Access `0x1800` | Device Name `0x2A00` | | read | `Triki <id>` |
| | Appearance `0x2A01` | | read | `0x0000` |
| Generic Attribute `0x1801` | Service Changed `0x2A05` | | indicate | |
| **Nordic UART (NUS)** | RX | `6e400002-…` | write / write-no-resp | komendy telefon→urz. |
| baza `6e400001-…` | TX | `6e400003-…` | notify | strumień IMU urz.→telefon |
| | Vendor / LED | `6e400004-…` | read / write | bit 0 = LED (na M5; w grach nieużywane) |
| Battery `0x180F` | Battery Level `0x2A19` | | read / notify | `100` |
| Device Information `0x180A` | Firmware Revision `0x2A26` | | read | `"3.2.1-A"` |

Baza UUID NUS: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`.

### Brak parowania, handshake startowy

- **Link jest otwarty** — bez SMP, bondingu i szyfrowania (potwierdzone przechwytem:
  0 pakietów SMP, 0 zdarzeń Encryption Change). Nie implementuj parowania.
- Handshake jest **jednokierunkowy, bez czelendża**: telefon robi discovery → czyta
  firmware (`0x2A26`) i battery (`0x2A19`) → włącza notyfikacje na TX (zapis CCCD
  `0x2902` = `01 00`) → wysyła **jeden** zapis START na RX. Telefon nie odsyła nic w
  odpowiedzi na dane z TX.

### Komendy na RX (telefon → urządzenie)

| Rola | Bajty | Uwaga |
|---|---|---|
| START | `20 10 00 D0 07 34 00 03` | bajt #5 bywa parametrem trybu (spec miał `0x68`); **rozpoznawaj po prefiksie `20 10`** |
| STOP | `20 00 00 00 00 00 00` | prefiks `20 00` |

Po STARCIE wyślij **raz** ramkę „ready" `21 00 00 00 00` (5 B, nagłówek `0x21`),
potem ciągły strumień IMU. STOP kończy strumień.

### Format ramki IMU (TX, notify)

Ramka **14 bajtów**:

```text
offset 0    1     2..3   4..5   6..7   8..9   10..11 12..13
       0x22 stat  gyroX  gyroY  gyroZ  accelX accelY accelZ
```

- bajt 0: stały nagłówek `0x22`. bajt 1: status, bit 0 = przycisk (`0x00`/`0x01`;
  w grach nieużywany, trzymaj `0x00`).
- bajty 2–13: 6× `int16` **little-endian ze znakiem** (gyro XYZ, accel XYZ).
- Skale (LSM6DSL): gyro **131.0** LSB/(deg/s), accel **2048.0** LSB/g. Wartość int16 =
  round(jednostka_fizyczna × skala), przycięta do zakresu int16 (kapsel też klipuje
  przy ±250 °/s i ±16 g). Brak licznika sekwencji / CRC / znacznika czasu.

### Charakterystyka strumienia (odtworzyć wiernie)

- ~**100 Hz** ramek (zmierzono ~94–104 Hz).
- Strumień to **ciągły ciąg bajtów** pocięty na notyfikacje w powtarzalnym cyklu
  **20 / 20 / 2 B = 42 B = dokładnie 3 ramki**. **Ramki przechodzą przez granice
  notyfikacji** — nie wysyłaj „1 ramka = 1 notyfikacja".
- |accel| w spoczynku ≈ 1 g (kontrola skali).

---

## Pułapki implementacyjne (ESP32 + NimBLE — kosztowały czas)

- **Adres random static:** `ble_hs_id_set_rnd(addr_le)` (bajty **LSB-first**). Ale
  `NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM)` w NimBLE-Arduino **włącza
  prywatność (RPA)** → urządzenie reklamuje losowy adres prywatny zamiast Twojego.
  Trzeba **wyłączyć prywatność**: `ble_hs_pvcy_rpa_config(0)`. Kolejność:
  `setOwnAddrType(RANDOM)` → `ble_hs_pvcy_rpa_config(0)` → `ble_hs_id_set_rnd(addr)`.
- **16-bit UUID `0x0001` w scan response:** `setCompleteServices(NimBLEUUID((uint16_t)0x0001))`.
- **Płynne ~100 Hz:** poproś o krótki interwał połączenia w `onConnect`
  (`updateConnParams(handle, 6, 12, 0, 200)` = 7.5–15 ms).
- **Notyfikacje 20/20/2:** akumuluj 42 B (3 ramki) i wyślij 3 notyfikacje (20, 20, 2 B).
- **M5StickC Plus2 / flash:** `board = m5stick-c` (4 MB; górne 4 MB chipa nieużywane).
  **Nie** wymuszaj 8 MB flash/partycji — niezgodność z 4 MB bootloaderem daje pętlę
  resetów (SW_RESET zaraz po `entry`, bez logów aplikacji). M5Unified wykrywa Plus2 w runtime.
- **CPU 80 MHz** wystarcza dla BLE + 100 Hz i mniej grzeje.

---

## Budowanie i wgrywanie

PlatformIO Core (CLI) lub wtyczka PlatformIO IDE. Z katalogu `firmware/`:

```bash
pio run                              # buduje domyslne env (m5stickc_plus2)
pio run -e esp32dev                  # buduje wersje na gole ESP32
pio run -e m5stickc_plus2 -t upload  # wgrywa na M5StickC Plus2
pio run -e esp32dev -t upload        # wgrywa na gole ESP32 (DevKit)
pio device monitor                   # log szeregowy (115200)
```

> Inne ESP32: dodaj własne `env` z odpowiednim `board` (rdzeń bez `-D HAS_M5`).
> Pierwszy build pobiera toolchain ESP32 (kilka minut).

## Sterowanie z PC (USB-serial) — działa na każdej płytce

Protokół linii (zakończone `\n`):

| Komenda | Działanie |
|---|---|
| `BLE,1` / `BLE,0` | włącz / wyłącz reklamę BLE (parowanie) |
| `M,gx,gy,gz,ax,ay,az` | ustaw ruch (gyro °/s, accel g) — włącza tryb „ruch z PC" |
| `R` | powrót do źródła domyślnego (IMU na M5; spoczynek na gołym ESP32) |

Gotowe narzędzia PC (`pc_control`, `pc_keyboard` ze sterowaniem klawiaturą, `pc_motion_file`,
`pc_motion_feed`, `pc_replay_capture`) oraz format pliku ruchu CSV są opisane w
[głównym README](../README.md). Otwierają port bez resetu płytki i **same wykrywają COM**
(po VID:PID mostka USB-serial ESP32; `--port` tylko przy kilku płytkach). Wspólny helper:
`../tools/_serial_util.py`.

Na gołym ESP32 reklama startuje **od razu** po włączeniu (brak przycisków) — z PC
wyłączasz przez `BLE,0`. Na M5 startuje w IDLE (przycisk A włącza).

## Sterowanie na M5StickC Plus2 (przyciski / zasilanie / ekran)

- **Przycisk A** (przedni): włącz/wyłącz reklamę BLE. Start w trybie **IDLE**.
- **Przycisk B** przytrzymany **2 s**: uśpienie (deep sleep, ~µA — urządzenie stygnie).
  **Wybudzenie: przycisk A.** (Nie jest używane `M5.Power.powerOff()` — na USB zostawiało
  płytkę w stanie nie do włączenia.)
- **Dowolny przycisk** budzi przygaszony ekran.
- Ekran: wersja `TrikiEmu`, stan BLE (off/reklama/połączony/strumień), nazwa, źródło
  ruchu (PC/IMU), bateria.
- Oszczędzanie/ciepło: CPU 80 MHz, niska jasność, ekran gaśnie po 30 s bezczynności,
  reklama bez połączenia wraca do IDLE po 3 min.

## Walidacja: „test TrikiScope" (przed Żappką)

Najpierw własny znany-dobry central, nie appka Żabki — to w 100% własny sprzęt/soft i
odsiewa większość błędów emulacji:

1. Włącz reklamę (przycisk A na M5 albo `pc_control.py ble-on`).
2. Uruchom **[TrikiScope](https://github.com/Maku-hub/TrikiScope)** → zobaczy urządzenie, połączy się, wyenumeruje GATT.
3. TrikiScope wysyła START → strumień rusza. Podaj ruch (z PC lub realnym IMU na M5).
4. Sprawdź, że dekoduje sensowne gyro (°/s) i accel (g), |accel| ≈ 1 g w spoczynku.
5. Dopiero potem próba z **Żappką**: znajduje → łączy → reaguje na ruch. Przy problemach:
   HCI snoop i porównanie sekwencji z prawdziwym kapslem (parser `../tools/parse_btsnoop.py`).
