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
//
// M5StickC Plus2 (HAS_M5) dodatkowo: A = reklama on/off, B(2s) = deep sleep (wybudzenie A).

#include <NimBLEDevice.h>
#include <math.h>

// Funkcje hosta NimBLE (linkowane z biblioteki): ustawienie statycznego adresu
// random oraz konfiguracja prywatnosci (0 = wylacz RPA -> uzyj statycznego adresu).
extern "C" int ble_hs_id_set_rnd(const uint8_t *rnd_addr);
extern "C" int ble_hs_pvcy_rpa_config(uint8_t enabled);

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
static const char* TRIKIEMU_VERSION = "1.0.0";

// ---- Tozsamosc (z prawdziwego kapsla) ----
// DEVICE_NAME: numer (Device ID) to przyklad — sama nazwa/numer nie sa krytyczne.
static const char* DEVICE_NAME   = "Triki 1234567890";
static const char* FIRMWARE_REV  = "3.2.1-A";

// Adres BLE prawdziwego kapsla — RANDOM STATIC: EC:07:A9:B8:C1:B7.
// MAC JEST WYMAGANY: Zappka rozpoznaje zapamietany kapsel po adresie — emulator z
// innym MAC dostaje "nie znaleziono triki" (sprawdzone: przykladowy adres nie dziala).
// Chcac uzyc wlasnego kapsla, wpisz tu jego adres. NimBLE: bajty LSB-first.
static uint8_t BLE_ADDR_LE[6] = {0xB7, 0xC1, 0xB8, 0xA9, 0x07, 0xEC};

// Adres MAC jako tekst do logu/ekranu (BLE_ADDR_LE jest LSB-first, wiec odwracamy).
static const char* macStr() {
  static char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           BLE_ADDR_LE[5], BLE_ADDR_LE[4], BLE_ADDR_LE[3],
           BLE_ADDR_LE[2], BLE_ADDR_LE[1], BLE_ADDR_LE[0]);
  return buf;
}

// ---- Skale sprzetowe oczekiwane przez format ramki (LSM6DSL) ----
static const float GYRO_SCALE  = 131.0f;   // LSB/(deg/s)  -> ~±250 dps full scale
static const float ACCEL_SCALE = 2048.0f;  // LSB/g        -> ~±16 g full scale

// ---- Komendy na RX ----
// START: 20 10 00 D0 07 34 00 03   STOP: 20 00 00 00 00 00 00
static const uint8_t CMD_START_PREFIX[] = {0x20, 0x10};
static const uint8_t CMD_STOP_PREFIX[]  = {0x20, 0x00};

// ---- Stan globalny (wspolny) ----
static NimBLECharacteristic* txChar  = nullptr;
static NimBLECharacteristic* batChar = nullptr;
static NimBLEServer*         server  = nullptr;

static volatile bool deviceConnected = false;
static volatile bool streaming       = false;
static uint16_t curConnHandle = 0xFFFF; // uchwyt biezacego polaczenia (do rozlaczenia)

static uint8_t streamAcc[42];   // bufor 3 ramek
static int     streamAccLen = 0;
static uint32_t lastFrameUs  = 0;
static const uint32_t FRAME_PERIOD_US = 10000; // 100 Hz

// Tryb reklamy. MODE_IDLE: nie reklamuje sie. MODE_ADV: reklama (parowanie) wlaczona.
enum AppMode { MODE_IDLE, MODE_ADV };
static AppMode appMode = MODE_IDLE;
static uint32_t advStartedMs = 0;

// Ruch sterowany z PC (USB-serial). Gdy gSerialMotion=true, pakujemy te wartosci.
static volatile float gMotion[6] = {0, 0, 0, 0, 0, 1.0f}; // spoczynek: az=1g
static volatile bool  gSerialMotion = false;
static char lineBuf[80];
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
  M5.Display.setTextColor(TFT_DARKGREY);
  M5.Display.printf("%s\n", macStr());
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE);
  if (gSerialMotion) M5.Display.println("ruch: z PC");

  int lvl = M5.Power.getBatteryLevel();
  if (lvl >= 0) M5.Display.printf("bat: %d%%\n", lvl);

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_DARKGREY);
  M5.Display.setCursor(0, M5.Display.height() - 16);
  M5.Display.println(appMode == MODE_IDLE ? "A: wlacz BLE" : "A: wylacz BLE");
  M5.Display.print("B (2s): uspij");
  M5.Display.setTextSize(2);
}
#else
static inline void drawStatus() {}  // brak ekranu na golym ESP32
#endif

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
    drawStatus();
  }
  void onDisconnect(NimBLEServer* s) override {
    deviceConnected = false;
    streaming = false;
    streamAccLen = 0;
    curConnHandle = 0xFFFF;
    if (appMode == MODE_ADV) {
      Serial.println("[TrikiEmu] ROZLACZONY -> wznawiam reklame");
      NimBLEDevice::startAdvertising();
      advStartedMs = millis();
    } else {
      Serial.println("[TrikiEmu] ROZLACZONY");
    }
    drawStatus();
  }
};

static void startStreaming() {
  streamAccLen = 0;
  lastFrameUs = micros();
  const uint8_t ready[5] = {0x21, 0x00, 0x00, 0x00, 0x00}; // ramka "ready" jak kapsel
  if (txChar) { txChar->setValue(ready, sizeof(ready)); txChar->notify(); }
  streaming = true;
  Serial.println("[TrikiEmu] START -> strumien ON");
  drawStatus();
}

static void stopStreaming() {
  streaming = false;
  streamAccLen = 0;
  drawStatus();
}

class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.size() < 2) return;
    const uint8_t b0 = (uint8_t)v[0], b1 = (uint8_t)v[1];
    if (b0 == CMD_START_PREFIX[0] && b1 == CMD_START_PREFIX[1]) {
      startStreaming();
    } else if (b0 == CMD_STOP_PREFIX[0] && b1 == CMD_STOP_PREFIX[1]) {
      stopStreaming();
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
  ble_hs_pvcy_rpa_config(0); // 0 = wylacz RPA -> uzyj statycznego adresu random
  int rc = ble_hs_id_set_rnd(BLE_ADDR_LE);
  Serial.printf("[TrikiEmu] adres random static rc=%d (%s)\n", rc, macStr());

  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

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
    NimBLEDevice::stopAdvertising();
    if (deviceConnected && curConnHandle != 0xFFFF && server) {
      server->disconnect(curConnHandle);
    }
    Serial.println("[TrikiEmu] BLE reklama OFF");
  }
  drawStatus();
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
                   &vv[0], &vv[1], &vv[2], &vv[3], &vv[4], &vv[5]) == 6) {
          for (int i = 0; i < 6; i++) gMotion[i] = vv[i];
          gSerialMotion = true;
        } else if (lineBuf[0] == 'R') {
          gSerialMotion = false;
        } else if (strncmp(lineBuf, "BLE,", 4) == 0) {
          setAdvertising(lineBuf[4] == '1');
        }
        lineLen = 0;
      }
    } else if (lineLen < (int)sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// Wysyla skumulowane 42 B jako notyfikacje w cyklu 20/20/2 (jak prawdziwy kapsel).
static void flushStream() {
  if (!txChar || !deviceConnected) return;
  txChar->setValue(streamAcc,      20); txChar->notify();
  txChar->setValue(streamAcc + 20, 20); txChar->notify();
  txChar->setValue(streamAcc + 40, 2);  txChar->notify();
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

void loop() {
#ifdef HAS_M5
  M5.update();
#endif
  pollSerialMotion();

#ifdef HAS_M5
  // --- Przyciski ---
  if (M5.BtnA.wasPressed()) {
    if (!screenOn) { setScreen(true); }
    else { setAdvertising(appMode == MODE_IDLE); }
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
    uint32_t now = micros();
    if ((int32_t)(now - lastFrameUs) >= (int32_t)FRAME_PERIOD_US) {
      lastFrameUs += FRAME_PERIOD_US;

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
      // status: bit0 = przycisk. Niewykorzystywany w grach -> 0x00.
      buildFrame(streamAcc + streamAccLen, 0x00, gx, gy, gz, ax, ay, az);
      streamAccLen += 14;

      if (streamAccLen >= 42) {
        flushStream();
        streamAccLen = 0;
      }
    }
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
