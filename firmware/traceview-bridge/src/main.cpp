// TraceView Truck Bridge v0.1 — M5Stack CoreS3 + CANBus Unit (U085, CA-IS3050G) on PORT C
//
// What it does
//  - Listens to the truck's J1939 bus (LISTEN-ONLY: it never transmits, never ACKs).
//  - Finds the bus speed by itself (250k / 500k) and the Grove wire order.
//  - Decodes active fault codes (DM1, incl. multi-packet BAM), lamps, RPM, battery volts,
//    coolant temp, oil pressure, road speed and VIN when the truck broadcasts it.
//  - Shows it on the CoreS3 touchscreen and streams it to the TraceView phone app over BLE.
//  - Demo mode (no truck needed) to test the phone link at home.
//
// Wiring: CANBus Unit → CoreS3 PORT C (blue, G17/G18) with the Grove cable.
//         CANBus Unit H / L screw terminals → truck 9-pin Deutsch pin C (CAN-H) / pin D (CAN-L).
//         Do not connect truck +12 V or ground to the CoreS3.

#include <M5Unified.h>
#include "driver/twai.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "j1939.h"

// ---------- config ----------
static const gpio_num_t PORTC_YELLOW = GPIO_NUM_17;
static const gpio_num_t PORTC_WHITE  = GPIO_NUM_18;
#define SVC_UUID  "7a1c0001-5e2b-4b8e-9c1a-74726163ee01"   // TraceView bridge service
#define TX_UUID   "7a1c0002-5e2b-4b8e-9c1a-74726163ee01"   // notify: JSON lines → phone
#define RX_UUID   "7a1c0003-5e2b-4b8e-9c1a-74726163ee01"   // write: commands ← phone

// ---------- shared state ----------
static j1939::Decoder D;
static SemaphoreHandle_t lock;
enum BusState { BUS_SCANNING, BUS_OK, BUS_NONE };
static volatile BusState busState = BUS_SCANNING;
static volatile uint32_t busBaud = 0, lastFrameMs = 0, fps = 0;
static volatile bool pinsSwapped = false, demo = false, wantRescan = false;
static volatile bool bleConnected = false;
static BLECharacteristic* txChar = nullptr;
static int tab = 0;   // 0 faults, 1 live, 2 bus

// ======================================================================
//  CAN (TWAI) — listen only
// ======================================================================
static bool twaiUp = false;
static void twaiDown() { if (twaiUp) { twai_stop(); twai_driver_uninstall(); twaiUp = false; } }

static bool twaiStart(uint32_t baud, bool swap) {
  twaiDown();
  gpio_num_t tx = swap ? PORTC_WHITE : PORTC_YELLOW, rx = swap ? PORTC_YELLOW : PORTC_WHITE;
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 64;
  twai_timing_config_t t250 = TWAI_TIMING_CONFIG_250KBITS();
  twai_timing_config_t t500 = TWAI_TIMING_CONFIG_500KBITS();
  twai_timing_config_t t = (baud == 500000) ? t500 : t250;
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
  if (twai_start() != ESP_OK) { twai_driver_uninstall(); return false; }
  twaiUp = true;
  return true;
}

// listen for a moment; a good setting gives clean 29-bit frames
static int probe(uint32_t ms) {
  int good = 0; uint32_t end = millis() + ms; twai_message_t m;
  while ((int32_t)(end - millis()) > 0) {
    if (twai_receive(&m, pdMS_TO_TICKS(20)) == ESP_OK && m.extd && !m.rtr) good++;
  }
  return good;
}

static void scanBus() {
  busState = BUS_SCANNING;
  const uint32_t bauds[] = {250000, 500000};
  for (int s = 0; s < 2; s++) for (uint32_t b : bauds) {
    if (demo) { twaiDown(); return; }
    if (!twaiStart(b, s)) continue;
    if (probe(1500) >= 5) { busBaud = b; pinsSwapped = s; busState = BUS_OK; lastFrameMs = millis(); return; }
  }
  twaiDown(); busBaud = 0; busState = BUS_NONE;
}

static void canTask(void*) {
  uint32_t winStart = millis(), winCount = 0, nextScan = 0;
  for (;;) {
    if (demo) { twaiDown(); busState = BUS_NONE; vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    if (wantRescan) { wantRescan = false; nextScan = 0; busState = BUS_SCANNING; }
    if (busState != BUS_OK) {
      if (millis() >= nextScan) { scanBus(); nextScan = millis() + 8000; }
      else vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(50)) == ESP_OK) {
      if (m.extd && !m.rtr) {
        xSemaphoreTake(lock, portMAX_DELAY);
        D.onFrame(m.identifier, true, m.data, m.data_length_code, millis());
        xSemaphoreGive(lock);
        lastFrameMs = millis(); winCount++;
      }
    }
    if (millis() - winStart >= 1000) { fps = winCount; winCount = 0; winStart = millis(); }
    if (millis() - lastFrameMs > 3000) { busState = BUS_SCANNING; nextScan = 0; fps = 0; }   // key off / unplugged
  }
}

// ======================================================================
//  Demo traffic — fake frames go through the same decoder
// ======================================================================
static uint32_t mkId(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da = 0xFF) {
  uint8_t pf = (pgn >> 8) & 0xFF;
  return ((uint32_t)prio << 26) | (((pgn >> 16) & 3) << 24) | ((uint32_t)pf << 16) | ((uint32_t)(pf < 240 ? da : (pgn & 0xFF)) << 8) | sa;
}
static void dtcBytes(uint8_t* o, uint32_t spn, uint8_t fmi, uint8_t oc) {
  o[0] = spn & 0xFF; o[1] = (spn >> 8) & 0xFF; o[2] = ((spn >> 11) & 0xE0) | (fmi & 0x1F); o[3] = oc & 0x7F;
}
static void demoTick() {
  static uint32_t next = 0; uint32_t now = millis();
  if (now < next) return; next = now + 250;
  float t = now / 1000.0f;
  uint8_t d[8];
  auto feed = [&](uint32_t id) { xSemaphoreTake(lock, portMAX_DELAY); D.onFrame(id, true, d, 8, now); xSemaphoreGive(lock); };
  uint16_t rpm = (uint16_t)((650 + 15 * sinf(t)) / 0.125f);
  memset(d, 0xFF, 8); d[3] = rpm & 0xFF; d[4] = rpm >> 8; feed(mkId(3, j1939::PGN_EEC1, 0));
  uint16_t bv = (uint16_t)((12.18f + 0.03f * sinf(t * 0.7f)) / 0.05f);
  memset(d, 0xFF, 8); d[4] = bv & 0xFF; d[5] = bv >> 8; d[6] = bv & 0xFF; d[7] = bv >> 8; feed(mkId(6, j1939::PGN_VEP1, 0));
  memset(d, 0xFF, 8); d[0] = 40 + 85; feed(mkId(6, j1939::PGN_ET1, 0));
  memset(d, 0xFF, 8); d[3] = 75; feed(mkId(6, j1939::PGN_EFLP1, 0));
  memset(d, 0xFF, 8); d[1] = 0; d[2] = 0; feed(mkId(6, j1939::PGN_CCVS1, 0));
  static uint32_t nextDm1 = 0;
  if (now >= nextDm1) { nextDm1 = now + 1000;   // engine: amber lamp, SPN 168 FMI 1 (matches the TraceView demo job)
    memset(d, 0xFF, 8); d[0] = 0x04; d[1] = 0xFF; dtcBytes(d + 2, 168, 1, 3); feed(mkId(6, j1939::PGN_DM1, 0)); }
  fps = 20;
}

// ======================================================================
//  BLE — JSON lines in 20-byte chunks (works without MTU negotiation)
// ======================================================================
class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; }
  void onDisconnect(BLEServer*) override { bleConnected = false; BLEDevice::startAdvertising(); }
};
class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String s = String(c->getValue().c_str()); s.trim();
    if (s == "demo on") demo = true;
    else if (s == "demo off") { demo = false; wantRescan = true; }
    else if (s == "rescan") wantRescan = true;
  }
};
static void bleSetup() {
  BLEDevice::init("TraceView-Bridge");
  BLEServer* srv = BLEDevice::createServer(); srv->setCallbacks(new SrvCb());
  BLEService* svc = srv->createService(SVC_UUID);
  txChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  txChar->addDescriptor(new BLE2902());
  BLECharacteristic* rx = svc->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID); adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}
static void bleSend(const char* line, int len) {
  if (!bleConnected || !txChar) return;
  for (int o = 0; o < len; o += 20) {
    int n = len - o < 20 ? len - o : 20;
    txChar->setValue((uint8_t*)line + o, n); txChar->notify(); delay(4);
  }
}
static void blePublish() {
  static uint32_t next = 0, lastSeq = 0xFFFFFFFF, nextCodes = 0;
  uint32_t now = millis(); if (now < next) return; next = now + 1000;
  char buf[900]; int n;
  n = snprintf(buf, sizeof buf, "{\"t\":\"st\",\"bus\":\"%s\",\"baud\":%lu,\"fps\":%lu,\"demo\":%d}\n",
               demo ? "demo" : busState == BUS_OK ? "ok" : busState == BUS_SCANNING ? "scanning" : "none",
               (unsigned long)busBaud, (unsigned long)fps, demo ? 1 : 0);
  bleSend(buf, n);
  xSemaphoreTake(lock, portMAX_DELAY);
  D.expire(now);
  n = D.jsonLive(buf, sizeof buf, now);
  xSemaphoreGive(lock);
  bleSend(buf, n);
  xSemaphoreTake(lock, portMAX_DELAY);
  bool send = D.changeSeq != lastSeq || now >= nextCodes;
  if (send) { n = D.jsonCodes(buf, sizeof buf, now); lastSeq = D.changeSeq; nextCodes = now + 3000; }
  xSemaphoreGive(lock);
  if (send) bleSend(buf, n);
}

// ======================================================================
//  Screen (320×240 touch)
// ======================================================================
static M5Canvas cv(&M5.Display);
static const uint16_t BG = 0x0861, CARD = 0x10C4, LINE = 0x2967, INK = 0xEF7D, MUTED = 0x8C51,
                      BLUE = 0x3C5F, GREEN = 0x4F0E, AMBER = 0xFD20, RED = 0xF206;

static void pill(int x, int y, const char* s, uint16_t col) {
  cv.setFont(&fonts::Font0); int w = cv.textWidth(s) + 12;
  cv.fillRoundRect(x, y, w, 16, 8, col); cv.setTextColor(BG); cv.setTextDatum(middle_center); cv.drawString(s, x + w / 2, y + 8);
}

static void drawHeader(uint32_t now) {
  cv.fillRect(0, 0, 320, 30, CARD); cv.drawFastHLine(0, 30, 320, LINE);
  cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextColor(INK); cv.setTextDatum(middle_left);
  cv.drawString("TRACEVIEW BRIDGE", 8, 15);
  char b[32];
  if (demo) pill(180, 7, "DEMO", AMBER);
  else if (busState == BUS_OK) { snprintf(b, sizeof b, "%luk %lu/s", (unsigned long)(busBaud / 1000), (unsigned long)fps); pill(180, 7, b, GREEN); }
  else if (busState == BUS_SCANNING) pill(180, 7, "SCANNING", BLUE);
  else pill(180, 7, "NO BUS", RED);
  cv.fillCircle(306, 15, 6, bleConnected ? BLUE : LINE);
  cv.setFont(&fonts::Font0); cv.setTextColor(MUTED); cv.setTextDatum(middle_right); cv.drawString(bleConnected ? "PHONE" : "BLE", 297, 15);
}

static void drawTabs() {
  const char* n[] = {"FAULTS", "LIVE", "BUS"};
  cv.fillRect(0, 204, 320, 36, CARD); cv.drawFastHLine(0, 204, 320, LINE);
  for (int i = 0; i < 3; i++) {
    int x = i * 107;
    if (i == tab) cv.fillRoundRect(x + 6, 208, 95, 28, 8, BLUE);
    cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextColor(i == tab ? INK : MUTED); cv.setTextDatum(middle_center);
    cv.drawString(n[i], x + 53, 222);
  }
}

static void drawFaults(uint32_t now) {
  xSemaphoreTake(lock, portMAX_DELAY);
  D.expire(now);
  j1939::Lamps L = D.lamps(now); int n = D.nDtc; j1939::Dtc list[8]; int shown = n < 8 ? n : 8;
  for (int i = 0; i < shown; i++) list[i] = D.dtc[i];
  xSemaphoreGive(lock);
  int x = 8;
  auto lamp = [&](const char* s, bool on, uint16_t c) { cv.fillRoundRect(x, 38, 72, 18, 5, on ? c : LINE); cv.setFont(&fonts::Font0);
    cv.setTextColor(on ? BG : MUTED); cv.setTextDatum(middle_center); cv.drawString(s, x + 36, 47); x += 77; };
  lamp("MIL", L.mil, AMBER); lamp("AMBER", L.awl, AMBER); lamp("RED STOP", L.rsl, RED); lamp("PROTECT", L.pl, AMBER);
  if (!n) {
    cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(busState == BUS_OK || demo ? GREEN : MUTED); cv.setTextDatum(middle_center);
    cv.drawString(busState == BUS_OK || demo ? "No active codes" : "Waiting for truck", 160, 120);
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(MUTED);
    cv.drawString(busState == BUS_OK || demo ? "Active faults (DM1) appear here" : "Key on, plug into the 9-pin port", 160, 150);
    return;
  }
  int y = 62;
  for (int i = 0; i < shown && y < 196; i++) {
    const auto& d = list[i];
    cv.fillRoundRect(6, y, 308, 42, 6, CARD);
    cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextColor(INK); cv.setTextDatum(top_left);
    char b[64]; snprintf(b, sizeof b, "SPN %lu  FMI %u", (unsigned long)d.spn, d.fmi); cv.drawString(b, 12, y + 4);
    const char* sa = j1939::saName(d.sa); char sab[24]; if (!sa) { snprintf(sab, sizeof sab, "SA %u", d.sa); sa = sab; }
    cv.setFont(&fonts::Font0); cv.setTextColor(BLUE); cv.setTextDatum(top_right); cv.drawString(sa, 308, y + 6);
    snprintf(b, sizeof b, "x%u", d.oc); cv.setTextColor(MUTED); cv.drawString(b, 308, y + 18);
    const char* nm = j1939::spnName(d.spn);
    snprintf(b, sizeof b, "%s%s%s", nm ? nm : "", nm ? " - " : "", j1939::fmiText(d.fmi));
    cv.setTextColor(MUTED); cv.setTextDatum(top_left); cv.drawString(b, 12, y + 26);
    y += 46;
  }
  if (n > shown) { cv.setFont(&fonts::Font0); cv.setTextColor(MUTED); cv.setTextDatum(top_left); char b[24]; snprintf(b, sizeof b, "+%d more on the phone", n - shown); cv.drawString(b, 12, 194); }
}

static void drawLive(uint32_t now) {
  xSemaphoreTake(lock, portMAX_DELAY); j1939::Live L = D.live; xSemaphoreGive(lock);
  struct Row { const char* k; float v; uint32_t t; const char* fmt; const char* u; } rows[] = {
    {"Battery", L.batV, L.tBat, "%.2f", "V"}, {"Engine speed", L.rpm, L.tRpm, "%.0f", "rpm"},
    {"Coolant", L.coolF, L.tCool, "%.0f", "F"}, {"Oil pressure", L.oilPsi, L.tOil, "%.0f", "psi"}, {"Road speed", L.mph, L.tMph, "%.0f", "mph"}};
  int y = 38;
  for (auto& r : rows) {
    cv.fillRoundRect(6, y, 308, 28, 6, CARD);
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(MUTED); cv.setTextDatum(middle_left); cv.drawString(r.k, 14, y + 14);
    char b[24]; bool ok = r.t && now - r.t < 5000;
    if (ok) snprintf(b, sizeof b, r.fmt, r.v); else snprintf(b, sizeof b, "--");
    cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(ok ? INK : LINE); cv.setTextDatum(middle_right); cv.drawString(b, 268, y + 14);
    cv.setFont(&fonts::Font0); cv.setTextColor(MUTED); cv.setTextDatum(middle_left); cv.drawString(r.u, 274, y + 15);
    y += 32;
  }
  cv.setFont(&fonts::Font0); cv.setTextColor(MUTED); cv.setTextDatum(top_left);
  char b[40]; snprintf(b, sizeof b, "VIN %s", L.vin[0] ? L.vin : "(not broadcast - scan it on the phone)"); cv.drawString(b, 10, 196);
}

static int btnDemo[4] = {10, 150, 145, 40}, btnScan[4] = {165, 150, 145, 40};
static void button(const int* r, const char* s, bool on) {
  cv.fillRoundRect(r[0], r[1], r[2], r[3], 8, on ? AMBER : CARD); cv.drawRoundRect(r[0], r[1], r[2], r[3], 8, on ? AMBER : LINE);
  cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextColor(on ? BG : INK); cv.setTextDatum(middle_center); cv.drawString(s, r[0] + r[2] / 2, r[1] + r[3] / 2);
}
static void drawBus(uint32_t now) {
  xSemaphoreTake(lock, portMAX_DELAY); uint32_t fr = D.frames, dm = D.dm1Count; xSemaphoreGive(lock);
  char b[64]; int y = 40;
  auto line = [&](const char* k, const char* v) { cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(MUTED); cv.setTextDatum(top_left); cv.drawString(k, 12, y);
    cv.setTextColor(INK); cv.setTextDatum(top_right); cv.drawString(v, 308, y); y += 21; };
  line("Mode", "Listen-only (never transmits)");
  snprintf(b, sizeof b, "%s", demo ? "Demo traffic" : busState == BUS_OK ? (busBaud == 500000 ? "500 kbps" : "250 kbps") : busState == BUS_SCANNING ? "Scanning..." : "None found");
  line("Bus", b);
  snprintf(b, sizeof b, "Port C %s", pinsSwapped ? "(wires swapped)" : "(G18 RX)"); line("Wiring", b);
  snprintf(b, sizeof b, "%lu total / %lu per s", (unsigned long)fr, (unsigned long)fps); line("Frames", b);
  snprintf(b, sizeof b, "%lu", (unsigned long)dm); line("DM1 messages", b);
  button(btnDemo, demo ? "Demo: ON" : "Demo mode", demo);
  button(btnScan, "Rescan bus", false);
}

static bool hit(const int* r, int x, int y) { return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }

// ======================================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Power.setExtOutput(true);          // 5 V out on the Grove ports (powers the CAN Unit)
  M5.Display.setRotation(1);
  cv.setColorDepth(16); cv.createSprite(320, 240);
  lock = xSemaphoreCreateMutex();
  bleSetup();
  xTaskCreatePinnedToCore(canTask, "can", 6144, nullptr, 3, nullptr, 0);
}

void loop() {
  M5.update();
  uint32_t now = millis();
  if (demo) demoTick();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (t.y >= 204) { tab = t.x / 107; if (tab > 2) tab = 2; }
    else if (tab == 2 && hit(btnDemo, t.x, t.y)) { demo = !demo; if (!demo) wantRescan = true; }
    else if (tab == 2 && hit(btnScan, t.x, t.y)) wantRescan = true;
  }
  blePublish();
  static uint32_t nextDraw = 0;
  if (now >= nextDraw) {
    nextDraw = now + 200;
    cv.fillSprite(BG);
    drawHeader(now);
    if (tab == 0) drawFaults(now); else if (tab == 1) drawLive(now); else drawBus(now);
    drawTabs();
    cv.pushSprite(0, 0);
  }
  delay(5);
}
