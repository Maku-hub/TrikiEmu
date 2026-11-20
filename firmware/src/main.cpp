// TrikiEmu — emulator kapsla Triki (BLE peripheral) na ESP32.
//
// Cel: dzialac na DOWOLNYM ESP32 (Arduino + NimBLE). M5StickC Plus2 to tylko jedna
// z platform — jego ekran/przyciski/IMU/bateria sa OPCJONALNE (kompilowane przy
// -D HAS_M5). Na "golym" ESP32 sterowanie idzie w calosci z PC po USB-serial.
//
// Odtwarza kontrakt opisany w firmware/README.md (sekcja "Kontrakt protokolu"):
//   reklama "Triki <id>" + mfr 0xFF00/a0 0a; NUS (RX/TX/Vendor) + Battery + DIS (fw "3.2.1-A");
//   po START na RX -> strumien ramek 14 B (0x22+status+6x int16 LE) ~100 Hz, ciety 20/20/2.
//
// Protokol sterowania z PC (USB-serial, linie zakonczone \n):
//   M,gx,gy,gz,ax,ay,az   ustaw ruch (gyro deg/s, accel g); wlacza tryb "ruch z PC"
//   R                     powrot do zrodla domyslnego (IMU na M5, spoczynek na golym ESP32)
//   BLE,1 / BLE,0         wlacz/wylacz reklame BLE (parowanie) — wazne na plytkach bez przyciskow
//   ADDR                  pokaz adres BLE (MAC) i czy jest skonfigurowany
//   ADDR,XX:XX:XX:XX:XX:XX  ustaw MAC kapsla (random static), zapis w NVS + restart
//   B                     klikniecie przycisku kapsla (~150 ms, bit 0 bajtu status w ramce)
//   B,1 / B,0             przycisk kapsla wcisniety / puszczony (przytrzymanie)
//
// M5StickC Plus2 (HAS_M5) dodatkowo: A = reklama on/off (w trakcie strumienia: przycisk
// kapsla), B(2s) = deep sleep (wybudzenie A).

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <math.h>

// Funkcje hosta NimBLE (linkowane z biblioteki): ustawienie statycznego adresu
// random oraz konfiguracja prywatnosci (0 = wylacz RPA -> uzyj statycznego adresu).
// Prywatnosc po stronie hosta jest tylko na klasycznym ESP32; na S3/C3 robi ja kontroler
// i setOwnAddrType() jej nie wlacza (funkcji ble_hs_pvcy_rpa_config tam nie ma).
extern "C" int ble_hs_id_set_rnd(const uint8_t *rnd_addr);
#if MYNEWT_VAL(BLE_HOST_BASED_PRIVACY)
extern "C" int ble_hs_pvcy_rpa_config(uint8_t enabled);
#endif

#ifdef HAS_M5
  #include <M5Unified.h>
  #include <esp_sleep.h>
#endif

// ---- Nordic UART Service (NUS) ----
static const char* NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // write (telefon->urzadzenie)
static const char* NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // notify (urzadzenie->telefon)
static const char* NUS_VENDOR  = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"; // read/write (bit0 = LED)

// ---- Wersja firmware TrikiEmu (rozna od emulowanej fw kapsla "3.2.1-A") ----
static const char* TRIKIEMU_VERSION = "1.2.0";

// ---- Tozsamosc (z prawdziwego kapsla) ----
// DEVICE_NAME: numer (Device ID) to przyklad — sama nazwa/numer nie sa krytyczne.
static const char* DEVICE_NAME   = "Triki 1234567890";
static const char* FIRMWARE_REV  = "3.2.1-A";

// Adres BLE (MAC) kapsla — RANDOM STATIC. MAC JEST WYMAGANY: Zappka rozpoznaje
// zapamietany kapsel po adresie — emulator z innym MAC dostaje "nie znaleziono triki".
// Adres jest per egzemplarz, wiec NIE jest wpisany w kod: ustawia sie go z PC komenda
// ADDR,XX:XX:XX:XX:XX:XX (zapis w NVS, przetrwa restart i deep sleep). Do tego czasu
// uzywany jest adres PRZYKLADOWY (wystarczy do testu TrikiScope, Zappka go nie przyjmie).
// NimBLE: bajty LSB-first.
static const uint8_t EXAMPLE_ADDR_LE[6] = {0x01, 0x00, 0x00, 0x00, 0xDE, 0xC0}; // C0:DE:00:00:00:01
static uint8_t BLE_ADDR_LE[6];
static bool    gAddrConfigured = false;
static const char* NVS_NS       = "trikiemu";
static const char* NVS_KEY_ADDR = "addr";

// Adres MAC jako tekst do logu/ekranu (BLE_ADDR_LE jest LSB-first, wiec odwracamy).
static const char* macStr() {
  static char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           BLE_ADDR_LE[5], BLE_ADDR_LE[4], BLE_ADDR_LE[3],
           BLE_ADDR_LE[2], BLE_ADDR_LE[1], BLE_ADDR_LE[0]);
  return buf;
}

// Parsuje "XX:XX:XX:XX:XX:XX" (lub z '-') do LSB-first i sprawdza, czy to poprawny
// adres random static: dwa najstarsze bity = 11, a pozostale 46 bitow nie same 0 ani 1.
static bool parseStaticAddr(const char* s, uint8_t out_le[6]) {
  unsigned v[6];
  char sep[5];
  if (sscanf(s, "%2x%c%2x%c%2x%c%2x%c%2x%c%2x", &v[0], &sep[0], &v[1], &sep[1], &v[2],
             &sep[2], &v[3], &sep[3], &v[4], &sep[4], &v[5]) != 11) return false;
  if (strlen(s) != 17) return false;
  for (int i = 0; i < 5; i++) if (sep[i] != ':' && sep[i] != '-') return false;
  if ((v[0] & 0xC0) != 0xC0) return false;
  bool allZero = (v[0] & 0x3F) == 0, allOne = (v[0] & 0x3F) == 0x3F;
  for (int i = 1; i < 6; i++) { allZero &= v[i] == 0x00; allOne &= v[i] == 0xFF; }
  if (allZero || allOne) return false;
  for (int i = 0; i < 6; i++) out_le[i] = (uint8_t)v[5 - i];
  return true;
}

// Wczytuje MAC z NVS; brak wpisu -> adres przykladowy.
static void loadAddr() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);   // RW: przy pierwszym starcie tworzy przestrzen (bez bledu w logu)
  gAddrConfigured = prefs.getBytes(NVS_KEY_ADDR, BLE_ADDR_LE, 6) == 6;
  prefs.end();
  if (!gAddrConfigured) memcpy(BLE_ADDR_LE, EXAMPLE_ADDR_LE, 6);
}

static void printAddr() {
  Serial.printf("[TrikiEmu] ADDR %s (%s)\n", macStr(),
                gAddrConfigured ? "skonfigurowany" : "PRZYKLADOWY - ustaw: ADDR,XX:XX:XX:XX:XX:XX");
}

// Komenda ADDR,<mac>: zapis w NVS i restart (adres BLE ustawia sie tylko przy starcie stosu).
static void setAddrFromSerial(const char* text) {
  uint8_t le[6];
  if (!parseStaticAddr(text, le)) {
    Serial.println("[TrikiEmu] ADDR BLAD: oczekiwano XX:XX:XX:XX:XX:XX, typ random static "
                   "(pierwszy bajt C0-FF)");
    return;
  }
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  bool ok = prefs.putBytes(NVS_KEY_ADDR, le, 6) == 6;
  prefs.end();
  if (!ok) { Serial.println("[TrikiEmu] ADDR BLAD: zapis do NVS nieudany"); return; }
  Serial.println("[TrikiEmu] ADDR zapisany -> restart");
  Serial.flush();
  delay(100);
  ESP.restart();
}

// ---- Skale sprzetowe oczekiwane przez format ramki (LSM6DSL) ----
// START ustawia ±16 g i ±2000 deg/s; gyro 70 mdps/LSB wg noty (pomiar na kapslu: 14.26).
static const float GYRO_SCALE  = 1000.0f / 70.0f; // 14.2857 LSB/(deg/s) -> ±2000 dps
static const float ACCEL_SCALE = 2048.0f;         // LSB/g -> ±16 g

// ---- Komendy na RX ----
// START: 20 | u16 LE zakres accel [g] | u16 LE zakres gyro [dps] | u16 LE ODR [Hz] | 03
//        Zappka: 20 10 00 D0 07 34 00 03 (16 g, 2000 dps, 52 Hz).
//        Parametry sa ignorowane: skale stale jak wyzej, strumien stale ~100 Hz.
// STOP:  20 00 00 00 00 00 00
static const uint8_t CMD_START_PREFIX[] = {0x20, 0x10};
static const uint8_t CMD_STOP_PREFIX[]  = {0x20, 0x00};

// ---- Stan globalny (wspolny) ----
static NimBLECharacteristic* txChar  = nullptr;
static NimBLECharacteristic* batChar = nullptr;
static NimBLEServer*         server  = nullptr;

static volatile bool deviceConnected = false;
static uint16_t curConnHandle = 0xFFFF; // uchwyt biezacego polaczenia (do rozlaczenia)

// Stan strumienia nalezy WYLACZNIE do loop(). Callbacki NimBLE (zadanie nimble_host,
// inny rdzen) tylko zglaszaja komende START/STOP w gStreamCmd; wygrywa ostatnia.
enum StreamCmd : uint8_t { STREAM_CMD_NONE, STREAM_CMD_START, STREAM_CMD_STOP };
static volatile uint8_t gStreamCmd = STREAM_CMD_NONE;
static bool    streaming    = false;
static uint8_t streamAcc[42];   // bufor 3 ramek
static int     streamAccLen = 0;
static uint32_t lastFrameUs  = 0;
static const uint32_t FRAME_PERIOD_US = 10000; // 100 Hz

// Kolejka wysylki na TX (tez tylko loop()): jedna cala wiadomosc/cykl ciety po 20 B.
static const int TX_CHUNK = 20;
static uint8_t  txPend[42];
static int      txPendLen = 0;      // dlugosc wiadomosci w kolejce (0 = pusto)
static int      txPendOff = 0;      // ile bajtow juz wyslano
static uint32_t txDropped = 0;      // pominiete cale wiadomosci/cykle (brak buforow)
static uint32_t txDroppedLogged = 0;

// Tryb reklamy. MODE_IDLE: nie reklamuje sie. MODE_ADV: reklama (parowanie) wlaczona.
enum AppMode { MODE_IDLE, MODE_ADV };
static AppMode appMode = MODE_IDLE;
static uint32_t advStartedMs = 0;

// Ruch sterowany z PC (USB-serial). Gdy gSerialMotion=true, pakujemy te wartosci.
static volatile float gMotion[6] = {0, 0, 0, 0, 0, 1.0f}; // spoczynek: az=1g
static volatile bool  gSerialMotion = false;
static char lineBuf[80];

// Przycisk kapsla (bit 0 bajtu status ramki IMU; kapsel nie wysyla osobnej wiadomosci).
// Zrodla: PC (B / B,1 / B,0) oraz na M5 przycisk A w trakcie strumienia. Tylko loop().
static const uint32_t BTN_TAP_MS = 150;
static bool     btnHeld      = false;   // B,1
static uint32_t btnTapUntil  = 0;       // koniec klikniecia (millis)
static bool     btnLast      = false;   // do logu zmian stanu
static int  lineLen = 0;

#ifdef HAS_M5
// ---- Stan/peryferia specyficzne dla M5StickC Plus2 ----
static uint32_t lastInteractMs = 0;
static bool     screenOn       = true;
static int      redLedPin      = 19;     // dioda GPIO19 — AKTYWNA STANEM WYSOKIM
static const uint8_t  SCREEN_BRIGHTNESS = 90;
static const uint32_t SCREEN_DIM_MS     = 30000;   // wygas ekran po 30 s bezczynnosci
static const uint32_t ADV_TIMEOUT_MS    = 180000;  // 3 min reklamy bez polaczenia -> idle
#endif

// Deklaracje wyprzedzajace
static void setAdvertising(bool on);

// ---------------------------------------------------------------------------
static inline int16_t clampToI16(float v) {
  if (v >  32767.0f) return  32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)lroundf(v);
}

// Buduje ramke 14 B w f[]. gyro w deg/s, accel w g.
static void buildFrame(uint8_t* f, uint8_t status,
                       float gx, float gy, float gz,
                       float ax, float ay, float az) {
  f[0] = 0x22;
  f[1] = status;
  int16_t vals[6] = {
    clampToI16(gx * GYRO_SCALE),  clampToI16(gy * GYRO_SCALE),  clampToI16(gz * GYRO_SCALE),
    clampToI16(ax * ACCEL_SCALE), clampToI16(ay * ACCEL_SCALE), clampToI16(az * ACCEL_SCALE),
  };
  // ESP32 jest little-endian -> kopiowanie daje int16 LE zgodnie ze spec.
  memcpy(f + 2, vals, sizeof(vals));
}

#ifdef HAS_M5
static void drawStatus() {
  if (!screenOn) return;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_GREEN);
  M5.Display.printf("TrikiEmu v%s\n", TRIKIEMU_VERSION);

  if (appMode == MODE_IDLE) {
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.println("BLE: WYL.");
  } else if (deviceConnected) {
    M5.Display.setTextColor(streaming ? TFT_CYAN : TFT_YELLOW);
    M5.Display.printf("BLE: %s\n", streaming ? "STRUMIEN" : "polaczony");
  } else {
    M5.Display.setTextColor(TFT_ORANGE);
    M5.Display.println("BLE: reklama...");
  }

  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.printf("%s\n", DEVICE_NAME);
  M5.Display.setTextSize(1);
  if (gAddrConfigured) {
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.printf("%s\n", macStr());
  } else {
    M5.Display.setTextColor(TFT_RED);
    M5.Display.printf("%s PRZYKLAD\nustaw MAC: ADDR,...\n", macStr());
  }
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE);
  if (gSerialMotion) M5.Display.println("ruch: z PC");

  int lvl = M5.Power.getBatteryLevel();
  if (lvl >= 0) M5.Display.printf("bat: %d%%\n", lvl);

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_DARKGREY);
  M5.Display.setCursor(0, M5.Display.height() - 16);
  M5.Display.println(streaming ? "A: przycisk kapsla"
                     : appMode == MODE_IDLE ? "A: wlacz BLE" : "A: wylacz BLE");
  M5.Display.print("B (2s): uspij");
  M5.Display.setTextSize(2);
}
#else
static inline void drawStatus() {}  // brak ekranu na golym ESP32
#endif

// Callbacki NimBLE dzialaja w zadaniu nimble_host (inny rdzen niz loop()), a ekran
// (SPI) nie jest reentrantny -> z callbackow tylko zglaszamy potrzebe przerysowania,
// rysuje wylacznie loop().
static volatile bool gRedraw = false;
static inline void requestRedraw() { gRedraw = true; }

// ---------------------------------------------------------------------------
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    deviceConnected = true;
    curConnHandle = desc->conn_handle;
    s->updateConnParams(desc->conn_handle, 6, 12, 0, 200); // 7.5–15 ms dla plynnego 100 Hz
    Serial.println("[TrikiEmu] central POLACZONY");
#ifdef HAS_M5
    lastInteractMs = millis();
#endif
    requestRedraw();
  }
  void onDisconnect(NimBLEServer* s) override {
    deviceConnected = false;
    gStreamCmd = STREAM_CMD_STOP;   // strumien zatrzyma loop()
    curConnHandle = 0xFFFF;
    if (appMode == MODE_ADV) {
      Serial.println("[TrikiEmu] ROZLACZONY -> wznawiam reklame");
      NimBLEDevice::startAdvertising();
      advStartedMs = millis();
    } else {
      Serial.println("[TrikiEmu] ROZLACZONY");
    }
    requestRedraw();
  }
};

// ---------------------------------------------------------------------------
// Wysylka na TX. NimBLECharacteristic::notify() w NimBLE-Arduino 1.4.x zwraca void i
// ignoruje blad hosta, wiec zgubiona notyfikacja bylaby niewidoczna, a urwany kawalek
// rozrywa ramke w strumieniu (brak licznika/CRC). Dlatego wysylamy sami i sprawdzamy rc:
//  - nie wyszedl PIERWSZY kawalek -> pomijamy cala wiadomosc/cykl (strumien zostaje spojny),
//  - nie wyszedl kolejny -> ponawiamy ten sam kawalek w nastepnych obiegach loop().
static bool sendChunk(const uint8_t* p, int n) {
  // (bez getSubscribedCount(): wektor subskrypcji zmienia zadanie nimble_host; START i tak
  //  przychodzi dopiero po wlaczeniu notyfikacji przez centrala)
  if (!txChar || curConnHandle == 0xFFFF) return false;
  os_mbuf* om = ble_hs_mbuf_from_flat(p, n);
  if (!om) return false;                                              // brak buforow
  return ble_gattc_notify_custom(curConnHandle, txChar->getHandle(), om) == 0; // om zuzyty zawsze
}

// Dosyla reszte wiadomosci z kolejki. true = kolejka pusta.
static bool pumpTx() {
  while (txPendOff < txPendLen) {
    int n = min(TX_CHUNK, txPendLen - txPendOff);
    if (!sendChunk(txPend + txPendOff, n)) return false;
    txPendOff += n;
  }
  txPendLen = txPendOff = 0;
  return true;
}

// Wstawia cala wiadomosc do kolejki i probuje wyslac. Gdy kolejka zajeta albo nie
// wyszedl zaden bajt -> wiadomosc pominieta w calosci (liczona w txDropped).
// mustSend (gotowosc 21): nie pomijamy, tylko ponawiamy — apka na nia czeka.
static void queueTx(const uint8_t* p, int n, bool mustSend = false) {
  if (txPendLen > 0) { txDropped++; return; }
  memcpy(txPend, p, n);
  txPendLen = n; txPendOff = 0;
  if (!pumpTx() && txPendOff == 0 && !mustSend) { txPendLen = 0; txDropped++; }
}

static void clearTx() { txPendLen = txPendOff = 0; }

// Wolane tylko z loop(). Gotowosc idzie jako osobna, cala wiadomosc na granicy
// cyklu (streamAccLen == 0), wiec nie rozrywa ramek w strumieniu.
static void startStreaming() {
  streamAccLen = 0;
  clearTx();
  txDropped = txDroppedLogged = 0;
  lastFrameUs = micros();
  const uint8_t ready[5] = {0x21, 0x00, 0x00, 0x00, 0x00}; // ramka "ready" jak kapsel
  queueTx(ready, sizeof(ready), true);
  streaming = true;
  Serial.println("[TrikiEmu] START -> strumien ON");
  drawStatus();
}

static void stopStreaming() {
  streaming = false;
  btnHeld = false; btnTapUntil = millis();   // nie zostawiaj przycisku wcisnietego na nastepna runde
  streamAccLen = 0;
  clearTx();
  Serial.printf("[TrikiEmu] STOP -> strumien OFF (pominiete cykle: %u)\n", (unsigned)txDropped);
  txDroppedLogged = txDropped;
  drawStatus();
}

// Odbiera komende zgloszona przez callback (atomowo: odczyt + wyczyszczenie).
// Powtorny START w trakcie strumienia jest ignorowany.
static void handleStreamCmd() {
  uint8_t cmd = __atomic_exchange_n(&gStreamCmd, (uint8_t)STREAM_CMD_NONE, __ATOMIC_ACQ_REL);
  if (cmd == STREAM_CMD_START && !streaming && deviceConnected) startStreaming();
  else if (cmd == STREAM_CMD_STOP && streaming) stopStreaming();
}

class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.size() < 2) return;
    const uint8_t b0 = (uint8_t)v[0], b1 = (uint8_t)v[1];
    if (b0 == CMD_START_PREFIX[0] && b1 == CMD_START_PREFIX[1]) {
      gStreamCmd = STREAM_CMD_START;   // obsluzy loop()
    } else if (b0 == CMD_STOP_PREFIX[0] && b1 == CMD_STOP_PREFIX[1]) {
      gStreamCmd = STREAM_CMD_STOP;
    }
  }
};

class VendorCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    bool ledOn = (uint8_t)v[0] & 0x01;             // bit 0 = LED
#ifdef HAS_M5
    digitalWrite(redLedPin, ledOn ? HIGH : LOW);   // Plus2: dioda aktywna stanem wysokim
#endif
    uint8_t masked = ledOn ? 0x01 : 0x00;
    c->setValue(&masked, 1);                        // maskowane do 1 bitu
  }
};

// ---------------------------------------------------------------------------
static void setupBle() {
  NimBLEDevice::init(DEVICE_NAME);

  // Adres BLE jak prawdziwy kapsel: RANDOM STATIC, BEZ prywatnosci (MAC jest wymagany).
  // setOwnAddrType(RANDOM) w NimBLE-Arduino wlacza RPA (adres prywatny), wiec
  // jawnie wylaczamy prywatnosc i ustawiamy nasz staly adres (BLE_ADDR_LE).
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
#if MYNEWT_VAL(BLE_HOST_BASED_PRIVACY)
  ble_hs_pvcy_rpa_config(0); // 0 = wylacz RPA -> uzyj statycznego adresu random
#endif
  int rc = ble_hs_id_set_rnd(BLE_ADDR_LE);
  Serial.printf("[TrikiEmu] adres random static rc=%d (%s)\n", rc, macStr());

  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  // NimBLE domyslnie sam wznawia reklame po rozlaczeniu — rowniez po BLE,0 / timeoucie,
  // gdy reklama ma byc WYLACZONA. Wznawianie robi nasz onDisconnect (tylko w MODE_ADV).
  server->advertiseOnDisconnect(false);

  // --- Nordic UART Service ---
  NimBLEService* nus = server->createService(NUS_SERVICE);
  NimBLECharacteristic* rx = nus->createCharacteristic(
      NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCB());

  txChar = nus->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);

  NimBLECharacteristic* vendor = nus->createCharacteristic(
      NUS_VENDOR, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  vendor->setCallbacks(new VendorCB());
  uint8_t ledInit = 0x00;
  vendor->setValue(&ledInit, 1);
  nus->start();

  // --- Battery Service (0x180F) ---
  NimBLEService* bat = server->createService((uint16_t)0x180F);
  batChar = bat->createCharacteristic(
      (uint16_t)0x2A19, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  uint8_t lvl = 100;
  batChar->setValue(&lvl, 1);
  bat->start();

  // --- Device Information (0x180A): tylko Firmware Revision, jak w spec ---
  NimBLEService* dis = server->createService((uint16_t)0x180A);
  NimBLECharacteristic* fw = dis->createCharacteristic(
      (uint16_t)0x2A26, NIMBLE_PROPERTY::READ);
  fw->setValue(std::string(FIRMWARE_REV));
  dis->start();

  // --- Reklama: nazwa + manufacturer data (company 0xFF00, payload a0 0a) ---
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setName(DEVICE_NAME);
  std::string mfg;
  mfg.push_back((char)0x00);  // company id LE: 0xFF00 -> 00 FF
  mfg.push_back((char)0xFF);
  mfg.push_back((char)0xA0);  // payload a0 0a
  mfg.push_back((char)0x0A);
  advData.setManufacturerData(mfg);
  adv->setAdvertisementData(advData);

  // Scan response: 16-bit Service Class UUID 0x0001 — prawdziwy kapsel to nadaje
  // (capture #01). Appka prawdopodobnie filtruje skan po tym UUID.
  NimBLEAdvertisementData scanData;
  scanData.setCompleteServices(NimBLEUUID((uint16_t)0x0001));
  adv->setScanResponseData(scanData);
  adv->setScanResponse(true);
  // Reklamy NIE startujemy tutaj — wlacza ja setAdvertising() (przyciskiem A lub z PC).
}

// Wlacza/wylacza reklame BLE (parowanie). Wylaczenie rozlacza tez aktywnego centrala.
static void setAdvertising(bool on) {
#ifdef HAS_M5
  lastInteractMs = millis();
#endif
  if (on) {
    appMode = MODE_ADV;
    NimBLEDevice::startAdvertising();
    advStartedMs = millis();
    Serial.println("[TrikiEmu] BLE reklama ON");
  } else {
    appMode = MODE_IDLE;
    streaming = false;
    streamAccLen = 0;
    clearTx();
    NimBLEDevice::stopAdvertising();
    if (deviceConnected && curConnHandle != 0xFFFF && server) {
      server->disconnect(curConnHandle);
    }
    Serial.println("[TrikiEmu] BLE reklama OFF");
  }
  drawStatus();
}

static bool allFinite(const float* v, int n) {
  for (int i = 0; i < n; i++) if (!isfinite(v[i])) return false;
  return true;
}

// Odbiera z PC linie sterujace (USB-serial): ruch (M/R) oraz reklame BLE (BLE,1/0).
static void pollSerialMotion() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        float vv[6];
        if (lineBuf[0] == 'M' &&
            sscanf(lineBuf + 1, ",%f,%f,%f,%f,%f,%f",
                   &vv[0], &vv[1], &vv[2], &vv[3], &vv[4], &vv[5]) == 6 &&
            allFinite(vv, 6)) {                       // nan/inf -> linia ignorowana
          for (int i = 0; i < 6; i++) gMotion[i] = vv[i];
          gSerialMotion = true;
        } else if (lineBuf[0] == 'R') {
          gSerialMotion = false;
          btnHeld = false;                            // powrot do stanu domyslnego
        } else if (strcmp(lineBuf, "B") == 0) {
          btnTapUntil = millis() + BTN_TAP_MS;        // kolejne B przedluza klikniecie
        } else if (strncmp(lineBuf, "B,", 2) == 0) {
          btnHeld = lineBuf[2] == '1';
          if (!btnHeld) btnTapUntil = millis();
        } else if (strncmp(lineBuf, "BLE,", 4) == 0) {
          setAdvertising(lineBuf[4] == '1');
        } else if (strncmp(lineBuf, "ADDR,", 5) == 0) {
          setAddrFromSerial(lineBuf + 5);
        } else if (strcmp(lineBuf, "ADDR") == 0) {
          printAddr();
        }
        lineLen = 0;
      }
    } else if (lineLen < (int)sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

#ifdef HAS_M5
// Uspienie = deep sleep (~uA, urzadzenie stygnie). Wybudzenie: przycisk A (GPIO37).
static void powerOff() {
  Serial.println("[TrikiEmu] -> deep sleep (wybudzenie: przycisk A)");
  NimBLEDevice::stopAdvertising();
  digitalWrite(redLedPin, LOW);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.println("Uspienie...");
  M5.Display.println("Wlacz: przycisk A");
  delay(900);
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)37, 0); // BtnA wcisniety = stan niski
  esp_deep_sleep_start();
}

static void setScreen(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  if (on) {
    M5.Display.wakeup();
    M5.Display.setBrightness(SCREEN_BRIGHTNESS);
    drawStatus();
  } else {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
  }
}
#endif

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  setCpuFrequencyMhz(80); // mniej ciepla; BLE i strumien 100 Hz w zupelnosci wystarcza

#ifdef HAS_M5
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.printf("[TrikiEmu] v%s boot (M5) board=%d imuEnabled=%d w=%d h=%d cpu=%dMHz\n",
                TRIKIEMU_VERSION, (int)M5.getBoard(), (int)M5.Imu.isEnabled(),
                (int)M5.Display.width(), (int)M5.Display.height(),
                (int)getCpuFrequencyMhz());
  M5.Display.setBrightness(SCREEN_BRIGHTNESS);
  M5.Display.setRotation(1);
  M5.Display.setTextSize(2);
  pinMode(redLedPin, OUTPUT);
  digitalWrite(redLedPin, LOW); // LED off (Plus2 GPIO19 aktywna stanem wysokim)
#else
  Serial.printf("[TrikiEmu] v%s boot (generic ESP32) cpu=%dMHz\n",
                TRIKIEMU_VERSION, (int)getCpuFrequencyMhz());
#endif

  loadAddr();
  printAddr();
  setupBle();

#ifdef HAS_M5
  // M5 ma przyciski -> start w IDLE, reklame wlacza przycisk A (lub PC: BLE,1).
  appMode = MODE_IDLE;
  lastInteractMs = millis();
  Serial.println("[TrikiEmu] gotowe (IDLE). A wlacza BLE; z PC: BLE,1 / BLE,0.");
  drawStatus();
#else
  // Goly ESP32: brak przyciskow -> startujemy reklame od razu; PC wylacza przez BLE,0.
  setAdvertising(true);
  Serial.println("[TrikiEmu] gotowe (reklama ON). Z PC: BLE,0 wylacza, BLE,1 wlacza.");
#endif
}

// Stan przycisku kapsla dla biezacej ramki.
static bool capButtonPressed() {
  bool on = btnHeld || (int32_t)(btnTapUntil - millis()) > 0;
#ifdef HAS_M5
  on = on || (streaming && M5.BtnA.isPressed());
#endif
  return on;
}

void loop() {
#ifdef HAS_M5
  M5.update();
#endif
  pollSerialMotion();

  handleStreamCmd();
  if (gRedraw) { gRedraw = false; drawStatus(); }   // zgloszone z callbackow BLE

#ifdef HAS_M5
  // --- Przyciski ---
  if (M5.BtnA.wasPressed()) {
    if (!screenOn) { setScreen(true); }
    else if (!streaming) { setAdvertising(appMode == MODE_IDLE); }  // w strumieniu A = przycisk kapsla
    lastInteractMs = millis();
  }
  if (M5.BtnB.wasPressed()) { setScreen(true); lastInteractMs = millis(); }
  if (M5.BtnB.pressedFor(2000)) { powerOff(); }

  // Wygaszanie ekranu po bezczynnosci (gdy brak polaczenia)
  if (screenOn && !deviceConnected && (millis() - lastInteractMs > SCREEN_DIM_MS)) {
    setScreen(false);
  }
  // Auto-powrot do idle po 3 min reklamy bez polaczenia
  if (appMode == MODE_ADV && !deviceConnected &&
      (millis() - advStartedMs > ADV_TIMEOUT_MS)) {
    Serial.println("[TrikiEmu] reklama bez polaczenia 3 min -> IDLE");
    setAdvertising(false);
  }
#endif

  if (streaming && deviceConnected) {
    if (txPendLen > 0) pumpTx();   // dokoncz przerwany cykl, zanim pojdzie nastepny
    uint32_t now = micros();
    uint32_t late = now - lastFrameUs;
    if (late >= FRAME_PERIOD_US) {
      // Rowne ~100 Hz jak czujnik: po dluzszym przycieciu loop() nie nadrabiamy zaleglych
      // ramek seria (co ~1 ms), tylko je gubimy i wracamy do rytmu.
      if (late >= 3 * FRAME_PERIOD_US) lastFrameUs = now;
      else lastFrameUs += FRAME_PERIOD_US;

      float gx, gy, gz, ax, ay, az;
      if (gSerialMotion) {
        gx = gMotion[0]; gy = gMotion[1]; gz = gMotion[2];
        ax = gMotion[3]; ay = gMotion[4]; az = gMotion[5];
      } else {
        gx = 0; gy = 0; gz = 0; ax = 0; ay = 0; az = 1.0f; // domyslnie spoczynek
#ifdef HAS_M5
        if (M5.Imu.update()) {
          auto d = M5.Imu.getImuData();
          ax = d.accel.x; ay = d.accel.y; az = d.accel.z; // g
          gx = d.gyro.x;  gy = d.gyro.y;  gz = d.gyro.z;   // deg/s
        }
#endif
      }
      // status: bit0 = przycisk kapsla.
      bool btn = capButtonPressed();
      if (btn != btnLast) {
        btnLast = btn;
        Serial.println(btn ? "[TrikiEmu] przycisk WCISNIETY" : "[TrikiEmu] przycisk puszczony");
      }
      buildFrame(streamAcc + streamAccLen, btn ? 0x01 : 0x00, gx, gy, gz, ax, ay, az);
      streamAccLen += 14;

      if (streamAccLen >= 42) {
        queueTx(streamAcc, 42);    // na kablu: 20/20/2 B
        streamAccLen = 0;
      }
    }
  }

  // Utrata danych ma byc widoczna: raport pominietych cykli (najwyzej co ~1 s).
  static uint32_t lastDropLog = 0;
  if (txDropped != txDroppedLogged && millis() - lastDropLog > 1000) {
    lastDropLog = millis();
    Serial.printf("[TrikiEmu] UWAGA: pominiete cykle strumienia (brak buforow BLE): %u\n",
                  (unsigned)txDropped);
    txDroppedLogged = txDropped;
  }

  // Aktualizacja poziomu baterii co ~5 s.
  static uint32_t lastBat = 0;
  if (millis() - lastBat > 5000) {
    lastBat = millis();
#ifdef HAS_M5
    int lvl = M5.Power.getBatteryLevel();
    if (lvl < 0) lvl = 100;
    if (lvl > 100) lvl = 100;
    uint8_t b = (uint8_t)lvl;
#else
    uint8_t b = 100; // goly ESP32 — brak pomiaru baterii
#endif
    if (batChar) { batChar->setValue(&b, 1); if (deviceConnected) batChar->notify(); }
  }

  delay(1);
}
