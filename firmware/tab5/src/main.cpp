// TraceView Tab5 v0.1 — the handheld brain of the two-device product (Tab5 + glasses)
//
//  - Reads the truck's J1939 bus, LISTEN-ONLY, through the CANBus Unit (U085) on PORT A (G53/G54).
//  - Replaces the phone app: dashboard (fault codes, lamps, live data), guided job, glasses settings.
//  - Runs a Wi-Fi hotspot. Glasses connect over TCP port 7070 and get JSON lines (see firmware/shared/glasses_link.md).
//  - Glasses button / voice sends next / prev / panels back to the Tab5.
//  - Demo mode for bench testing without a truck.

#include <M5Unified.h>
#include <WiFi.h>
#include "driver/twai.h"
#include "j1939.h"

// ---------------- config ----------------
static const gpio_num_t PORTA_YELLOW = GPIO_NUM_53;
static const gpio_num_t PORTA_WHITE  = GPIO_NUM_54;
static const char* AP_PASS = "traceview1";      // 8+ chars; change for real use
static const uint16_t LINK_PORT = 7070;
// Tab5: ESP32-P4 <-> ESP32-C6 SDIO pins (from M5Stack's Tab5 pin map)
static const int C6_CLK = 12, C6_CMD = 13, C6_D0 = 11, C6_D1 = 10, C6_D2 = 9, C6_D3 = 8, C6_RST = 15;
// Tab5 IO expanders (PI4IOE5V6408): WLAN_PWR_EN = 0x44 P0, EXT5V_EN = 0x43 P2
static const uint8_t EXP1 = 0x43, EXP2 = 0x44;

// ---------------- shared state ----------------
static j1939::Decoder D;
static SemaphoreHandle_t lock;
enum BusState { BUS_SCANNING, BUS_OK, BUS_NONE };
static volatile BusState busState = BUS_SCANNING;
static volatile uint32_t busBaud = 0, lastFrameMs = 0, fps = 0;
static volatile bool pinsSwapped = false, demo = false, wantRescan = false;
static char apName[24] = "TraceView-Tab5";
static bool wifiUp = false;

// ---------------- guided job (same demo job as the TraceView web app) ----------------
struct Step { const char* title; const char* sub[3]; const char* comp; const char* torque; };
static const Step STEPS[] = {
  {"Check battery state of charge", {"Key off, lights off", "Measure across each battery", "Spec: 12.4 V or more (general practice)"}, "BAT", ""},
  {"Inspect main ground (GNDP)", {"Disconnect batteries first (SB 54-318)", "Clean stud and ring eyes to bare metal", "Torque ring eye to 26 ft-lb"}, "G1", "26 ft-lb"},
  {"Starter + side voltage drop", {"Red lead BAT+, black lead starter B+", "Crank the engine", "Spec: 0.5 V max (general practice)"}, "STR", ""},
  {"Starter - side voltage drop", {"Red lead starter case, black lead BAT-", "Crank the engine", "Spec: 0.5 V max (general practice)"}, "STR", ""},
  {"Torque starter terminals", {"B+ nut 18-20 lb-ft", "S nut 18-22 lb-in", "Ground nut 18-20 lb-ft"}, "STR", "B+ 18-20 lb-ft"},
  {"Verify charging output", {"Engine at fast idle", "Alternator B+ to ground", "Spec: 13.8-14.4 V (general practice)"}, "ALT", ""},
};
static const int NSTEPS = sizeof(STEPS) / sizeof(STEPS[0]);
static const char* JOB_TITLE = "Slow crank / no crank";
static const uint32_t JOB_SPN = 168;           // job offered when this SPN is active
struct Job { bool active = false, complete = false; int i = 0; } job;
static bool panelsOn = true; static bool panelsRight = true;
static volatile uint32_t jobSeq = 1, uiSeq = 1;  // bump to push to glasses

// ======================================================================
//  Tab5 power helpers (IO expander). M5Unified normally sets these; this makes sure.
// ======================================================================
static void expanderOutHigh(uint8_t addr, uint8_t bit) {
  // PI4IOE5V6408 registers: 0x03 direction (1 = output), 0x05 output, 0x07 high-Z (1 = high-Z)
  M5.In_I2C.bitOn(addr, 0x03, 1 << bit, 400000);
  M5.In_I2C.bitOff(addr, 0x07, 1 << bit, 400000);
  M5.In_I2C.bitOn(addr, 0x05, 1 << bit, 400000);
}

// ======================================================================
//  CAN — listen only (same logic as the CoreS3 bridge)
// ======================================================================
static bool twaiUp = false;
static void twaiDown() { if (twaiUp) { twai_stop(); twai_driver_uninstall(); twaiUp = false; } }
static bool twaiStart(uint32_t baud, bool swap) {
  twaiDown();
  gpio_num_t tx = swap ? PORTA_WHITE : PORTA_YELLOW, rx = swap ? PORTA_YELLOW : PORTA_WHITE;
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(tx, rx, TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 64;
  twai_timing_config_t t250 = TWAI_TIMING_CONFIG_250KBITS();
  twai_timing_config_t t500 = TWAI_TIMING_CONFIG_500KBITS();
  twai_timing_config_t t = (baud == 500000) ? t500 : t250;
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
  if (twai_start() != ESP_OK) { twai_driver_uninstall(); return false; }
  twaiUp = true; return true;
}
static int probe(uint32_t ms) {
  int good = 0; uint32_t end = millis() + ms; twai_message_t m;
  while ((int32_t)(end - millis()) > 0) if (twai_receive(&m, pdMS_TO_TICKS(20)) == ESP_OK && m.extd && !m.rtr) good++;
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
    if (busState != BUS_OK) { if (millis() >= nextScan) { scanBus(); nextScan = millis() + 8000; } else vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(50)) == ESP_OK && m.extd && !m.rtr) {
      xSemaphoreTake(lock, portMAX_DELAY); D.onFrame(m.identifier, true, m.data, m.data_length_code, millis()); xSemaphoreGive(lock);
      lastFrameMs = millis(); winCount++;
    }
    if (millis() - winStart >= 1000) { fps = winCount; winCount = 0; winStart = millis(); }
    if (millis() - lastFrameMs > 3000) { busState = BUS_SCANNING; nextScan = 0; fps = 0; }
  }
}

// ---------------- demo traffic through the same decoder ----------------
static uint32_t mkId(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da = 0xFF) {
  uint8_t pf = (pgn >> 8) & 0xFF;
  return ((uint32_t)prio << 26) | (((pgn >> 16) & 3) << 24) | ((uint32_t)pf << 16) | ((uint32_t)(pf < 240 ? da : (pgn & 0xFF)) << 8) | sa;
}
static void demoTick() {
  static uint32_t next = 0, nextDm1 = 0; uint32_t now = millis();
  if (now < next) return;
  next = now + 250;
  float t = now / 1000.0f; uint8_t d[8];
  auto feed = [&](uint32_t id) { xSemaphoreTake(lock, portMAX_DELAY); D.onFrame(id, true, d, 8, now); xSemaphoreGive(lock); };
  uint16_t rpm = (uint16_t)((650 + 15 * sinf(t)) / 0.125f);
  memset(d, 0xFF, 8); d[3] = rpm & 0xFF; d[4] = rpm >> 8; feed(mkId(3, j1939::PGN_EEC1, 0));
  uint16_t bv = (uint16_t)((12.18f + 0.03f * sinf(t * 0.7f)) / 0.05f);
  memset(d, 0xFF, 8); d[4] = bv & 0xFF; d[5] = bv >> 8; feed(mkId(6, j1939::PGN_VEP1, 0));
  memset(d, 0xFF, 8); d[0] = 40 + 85; feed(mkId(6, j1939::PGN_ET1, 0));
  memset(d, 0xFF, 8); d[3] = 75; feed(mkId(6, j1939::PGN_EFLP1, 0));
  memset(d, 0xFF, 8); d[1] = 0; d[2] = 0; feed(mkId(6, j1939::PGN_CCVS1, 0));
  if (now >= nextDm1) { nextDm1 = now + 1000;
    memset(d, 0xFF, 8); d[0] = 0x04; d[2] = 168 & 0xFF; d[3] = 0; d[4] = 1; d[5] = 3; feed(mkId(6, j1939::PGN_DM1, 0)); }
  fps = 20;
}

// ======================================================================
//  Job control (touch, glasses button, voice all land here)
// ======================================================================
static void jobStart() { job.active = true; job.complete = false; job.i = 0; jobSeq++; }
static void jobNext() { if (!job.active) return; if (job.i < NSTEPS - 1) job.i++; else { job.complete = true; job.active = false; } jobSeq++; }
static void jobPrev() { if (!job.active) return; if (job.i > 0) job.i--; jobSeq++; }
static void jobEnd() { job.active = false; job.complete = false; jobSeq++; }
static void togglePanels() { panelsOn = !panelsOn; uiSeq++; }

static void handleCommand(const String& raw) {
  String s = raw; s.trim(); s.toLowerCase();
  // accept {"t":"btn","a":"next"} / {"t":"voice","text":"next step"} / plain words
  int a = s.indexOf("\"a\":\""); if (a >= 0) { int e = s.indexOf('"', a + 5); s = s.substring(a + 5, e); }
  int v = s.indexOf("\"text\":\""); if (v >= 0) { int e = s.indexOf('"', v + 8); s = s.substring(v + 8, e); }
  if (s == "next" || s == "next step" || s == "continue" || s == "done") jobNext();
  else if (s == "prev" || s == "previous" || s == "previous step" || s == "back" || s == "go back") jobPrev();
  else if (s == "panels" || s == "toggle panels") togglePanels();
  else if (s == "hide panels") { panelsOn = false; uiSeq++; }
  else if (s == "show panels") { panelsOn = true; uiSeq++; }
  else if (s == "start job") jobStart();
}

// ======================================================================
//  Wi-Fi hotspot + glasses link (TCP 7070, JSON lines)
// ======================================================================
static WiFiServer server(LINK_PORT);
static const int MAXC = 4;
static WiFiClient clients[MAXC];
static String rxBuf[MAXC];

static void wifiSetup() {
  expanderOutHigh(EXP2, 0);                       // ESP32-C6 power on
  delay(100);
#if CONFIG_IDF_TARGET_ESP32P4
  WiFi.setPins(C6_CLK, C6_CMD, C6_D0, C6_D1, C6_D2, C6_D3, C6_RST);
#endif
  uint8_t mac[6] = {0}; WiFi.mode(WIFI_AP); WiFi.softAPmacAddress(mac);
  snprintf(apName, sizeof apName, "TraceView-%02X%02X", mac[4], mac[5]);
  wifiUp = WiFi.softAP(apName, AP_PASS);
  if (wifiUp) server.begin();
}

static int connectedGlasses() { int n = 0; for (auto& c : clients) if (c && c.connected()) n++; return n; }

static void sendAll(const char* line, int len) {
  for (auto& c : clients) if (c && c.connected()) c.write((const uint8_t*)line, len);
}
static void jsonEsc(char* out, int cap, const char* s) {
  int n = 0; for (; *s && n < cap - 2; s++) { if (*s == '"' || *s == '\\') out[n++] = '\\'; out[n++] = *s; } out[n] = 0;
}
static int jobJson(char* b, int cap) {
  if (!job.active) return snprintf(b, cap, "{\"t\":\"job\",\"active\":0,\"complete\":%d,\"title\":\"%s\"}\n", job.complete, JOB_TITLE);
  const Step& s = STEPS[job.i]; char e0[96], e1[96], e2[96], tt[96];
  jsonEsc(tt, 96, s.title); jsonEsc(e0, 96, s.sub[0]); jsonEsc(e1, 96, s.sub[1]); jsonEsc(e2, 96, s.sub[2]);
  return snprintf(b, cap, "{\"t\":\"job\",\"active\":1,\"title\":\"%s\",\"i\":%d,\"n\":%d,\"step\":\"%s\",\"sub\":[\"%s\",\"%s\",\"%s\"],\"comp\":\"%s\",\"torque\":\"%s\"}\n",
                  JOB_TITLE, job.i, NSTEPS, tt, e0, e1, e2, s.comp, s.torque);
}
static void sendHello(WiFiClient& c) {
  char b[256]; int n;
  n = snprintf(b, sizeof b, "{\"t\":\"hello\",\"v\":1,\"name\":\"%s\"}\n", apName); c.write((const uint8_t*)b, n);
  xSemaphoreTake(lock, portMAX_DELAY); const char* vin = D.live.vin[0] ? D.live.vin : ""; char vb[24]; strncpy(vb, vin, 23); vb[23] = 0; xSemaphoreGive(lock);
  n = snprintf(b, sizeof b, "{\"t\":\"veh\",\"name\":\"2026 Freightliner Cascadia 126\",\"vin\":\"%s\"}\n", vb); c.write((const uint8_t*)b, n);
}

static void linkTick() {
  if (!wifiUp) return;
  WiFiClient nc = server.accept();
  if (nc) { for (int i = 0; i < MAXC; i++) if (!clients[i] || !clients[i].connected()) { clients[i] = nc; rxBuf[i] = ""; sendHello(clients[i]); jobSeq++; uiSeq++; break; } }
  for (int i = 0; i < MAXC; i++) {
    WiFiClient& c = clients[i]; if (!c || !c.connected()) continue;
    while (c.available()) { char ch = (char)c.read(); if (ch == '\n') { handleCommand(rxBuf[i]); rxBuf[i] = ""; } else if (rxBuf[i].length() < 200) rxBuf[i] += ch; }
  }
  static uint32_t lastJob = 0, lastUi = 0, next = 0; char b[900]; int n;
  if (jobSeq != lastJob) { lastJob = jobSeq; n = jobJson(b, sizeof b); sendAll(b, n); }
  if (uiSeq != lastUi) { lastUi = uiSeq; n = snprintf(b, sizeof b, "{\"t\":\"ui\",\"panels\":%d,\"side\":\"%s\"}\n", panelsOn, panelsRight ? "right" : "left"); sendAll(b, n); }
  uint32_t now = millis(); if (now < next) return; next = now + 1000;
  xSemaphoreTake(lock, portMAX_DELAY); D.expire(now);
  n = D.jsonLive(b, sizeof b, now); sendAll(b, n);
  n = D.jsonCodes(b, sizeof b, now); sendAll(b, n);
  xSemaphoreGive(lock);
}

// ======================================================================
//  UI — 1280×720 landscape, touch
// ======================================================================
static M5Canvas cv(&M5.Display);
static int W = 1280, H = 720;
static int tab = 0;                    // 0 dashboard, 1 job, 2 glasses, 3 bus
static const uint16_t BG = 0x0861, CARD = 0x10C4, LINE = 0x2967, INK = 0xEF7D, MUTED = 0x8C51,
                      BLUE = 0x3C5F, GREEN = 0x4F0E, AMBER = 0xFD20, RED = 0xF206;
struct Btn { int x, y, w, h; int id; };
static Btn btns[24]; static int nb = 0;
enum { B_TAB0 = 1, B_TAB1, B_TAB2, B_TAB3, B_START, B_NEXT, B_PREV, B_END, B_DEMO, B_SCAN, B_PANELS, B_SIDE, B_JOBFROMCODE };

static void addBtn(int x, int y, int w, int h, int id) { if (nb < 24) btns[nb++] = {x, y, w, h, id}; }
static void button(int x, int y, int w, int h, const char* s, int id, uint16_t fill = CARD, uint16_t ink = INK) {
  cv.fillRoundRect(x, y, w, h, 14, fill); cv.drawRoundRect(x, y, w, h, 14, fill == CARD ? LINE : fill);
  cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(ink); cv.setTextDatum(middle_center); cv.drawString(s, x + w / 2, y + h / 2);
  addBtn(x, y, w, h, id);
}
static void pill(int x, int y, const char* s, uint16_t col) {
  cv.setFont(&fonts::FreeSansBold12pt7b); int w = cv.textWidth(s) + 28;
  cv.fillRoundRect(x, y, w, 36, 18, col); cv.setTextColor(BG); cv.setTextDatum(middle_center); cv.drawString(s, x + w / 2, y + 18);
}

static void drawHeader() {
  cv.fillRect(0, 0, W, 76, CARD); cv.drawFastHLine(0, 76, W, LINE);
  cv.setFont(&fonts::FreeSansBold24pt7b); cv.setTextColor(INK); cv.setTextDatum(middle_left); cv.drawString("TRACEVIEW", 24, 38);
  cv.setFont(&fonts::FreeSans12pt7b); cv.setTextColor(MUTED);
  xSemaphoreTake(lock, portMAX_DELAY); char vin[20]; strncpy(vin, D.live.vin, 19); vin[19] = 0; xSemaphoreGive(lock);
  char b[96]; snprintf(b, sizeof b, "2026 Freightliner Cascadia 126  %s", vin[0] ? vin : "(demo package)"); cv.drawString(b, 290, 38);
  char p[32]; int x = W - 520;
  if (demo) pill(x, 20, "DEMO DATA", AMBER);
  else if (busState == BUS_OK) { snprintf(p, sizeof p, "J1939 %luk  %lu/s", (unsigned long)(busBaud / 1000), (unsigned long)fps); pill(x, 20, p, GREEN); }
  else if (busState == BUS_SCANNING) pill(x, 20, "SCANNING BUS", BLUE); else pill(x, 20, "NO BUS", RED);
  int g = connectedGlasses(); snprintf(p, sizeof p, g ? "GLASSES %d" : "NO GLASSES", g); pill(W - 230, 20, p, g ? BLUE : LINE);
}
static void drawTabs() {
  const char* n[] = {"DASHBOARD", "JOB", "GLASSES", "BUS"}; int tw = W / 4;
  cv.fillRect(0, H - 86, W, 86, CARD); cv.drawFastHLine(0, H - 86, W, LINE);
  for (int i = 0; i < 4; i++) {
    int x = i * tw; if (i == tab) cv.fillRoundRect(x + 12, H - 76, tw - 24, 66, 14, BLUE);
    cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(i == tab ? INK : MUTED); cv.setTextDatum(middle_center); cv.drawString(n[i], x + tw / 2, H - 43);
    addBtn(x, H - 86, tw, 86, B_TAB0 + i);
  }
}

static void drawDashboard(uint32_t now) {
  xSemaphoreTake(lock, portMAX_DELAY); D.expire(now);
  j1939::Lamps L = D.lamps(now); int n = D.nDtc; j1939::Dtc list[6]; int shown = n < 6 ? n : 6;
  for (int i = 0; i < shown; i++) list[i] = D.dtc[i];
  j1939::Live lv = D.live; xSemaphoreGive(lock);
  // lamps
  int x = 24; const int y0 = 96;
  auto lamp = [&](const char* s, bool on, uint16_t c) { cv.fillRoundRect(x, y0, 170, 44, 10, on ? c : LINE); cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextColor(on ? BG : MUTED); cv.setTextDatum(middle_center); cv.drawString(s, x + 85, y0 + 22); x += 182; };
  lamp("MIL", L.mil, AMBER); lamp("AMBER", L.awl, AMBER); lamp("RED STOP", L.rsl, RED);
  // fault codes (left)
  cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(INK); cv.setTextDatum(top_left); cv.drawString("Active fault codes", 24, 160);
  int y = 206; bool jobMatch = false;
  if (!n) { cv.setFont(&fonts::FreeSans18pt7b); cv.setTextColor(busState == BUS_OK || demo ? GREEN : MUTED);
    cv.drawString(busState == BUS_OK || demo ? "None reported" : "Waiting for truck: key on, plug into the 9-pin port", 24, y); }
  for (int i = 0; i < shown && y < H - 190; i++) {
    const auto& d = list[i]; if (d.spn == JOB_SPN) jobMatch = true;
    cv.fillRoundRect(24, y, 700, 70, 12, CARD); cv.drawRoundRect(24, y, 700, 70, 12, AMBER);
    char b[96]; snprintf(b, sizeof b, "SPN %lu  FMI %u", (unsigned long)d.spn, d.fmi);
    cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(INK); cv.setTextDatum(top_left); cv.drawString(b, 40, y + 8);
    const char* sa = j1939::saName(d.sa); char sab[16]; if (!sa) { snprintf(sab, sizeof sab, "SA %u", d.sa); sa = sab; }
    cv.setFont(&fonts::FreeSans12pt7b); cv.setTextColor(BLUE); cv.setTextDatum(top_right); cv.drawString(sa, 708, y + 12);
    const char* nm = j1939::spnName(d.spn); snprintf(b, sizeof b, "%s%s%s  (x%u)", nm ? nm : "", nm ? " - " : "", j1939::fmiText(d.fmi), d.oc);
    cv.setTextColor(MUTED); cv.setTextDatum(top_left); cv.drawString(b, 40, y + 42);
    y += 80;
  }
  if (jobMatch && !job.active) button(24, H - 176, 700, 70, "Start job for SPN 168: Slow crank / no crank", B_JOBFROMCODE, BLUE);
  // live tiles (right)
  struct T { const char* k; float v; uint32_t t; const char* f; const char* u; } tiles[] = {
    {"Battery", lv.batV, lv.tBat, "%.2f", "V"}, {"Engine", lv.rpm, lv.tRpm, "%.0f", "rpm"}, {"Coolant", lv.coolF, lv.tCool, "%.0f", "F"},
    {"Oil", lv.oilPsi, lv.tOil, "%.0f", "psi"}, {"Speed", lv.mph, lv.tMph, "%.0f", "mph"}};
  for (int i = 0; i < 5; i++) {
    int tx = 760 + (i % 2) * 254, ty = 96 + (i / 2) * 170;
    cv.fillRoundRect(tx, ty, 240, 156, 14, CARD);
    cv.setFont(&fonts::FreeSans12pt7b); cv.setTextColor(MUTED); cv.setTextDatum(top_left); cv.drawString(tiles[i].k, tx + 18, ty + 14);
    char b[16]; bool ok = tiles[i].t && now - tiles[i].t < 5000; if (ok) snprintf(b, sizeof b, tiles[i].f, tiles[i].v); else snprintf(b, sizeof b, "--");
    cv.setFont(&fonts::FreeSansBold24pt7b); cv.setTextColor(ok ? INK : LINE); cv.drawString(b, tx + 18, ty + 56);
    cv.setFont(&fonts::FreeSans12pt7b); cv.setTextColor(MUTED); cv.drawString(tiles[i].u, tx + 18, ty + 116);
  }
}

static void drawJob() {
  cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(INK); cv.setTextDatum(top_left);
  cv.drawString(JOB_TITLE, 24, 96);
  cv.setFont(&fonts::FreeSans12pt7b); cv.setTextColor(MUTED); cv.drawString("Starting & charging - SPN 168 FMI 1 (battery potential low)", 24, 136);
  // step list (left)
  for (int i = 0; i < NSTEPS; i++) {
    int y = 176 + i * 66; bool cur = job.active && i == job.i, done = job.complete || (job.active && i < job.i);
    cv.fillRoundRect(24, y, 470, 58, 12, cur ? BLUE : CARD);
    cv.fillCircle(56, y + 29, 17, done ? GREEN : (cur ? INK : LINE));
    char b[4]; snprintf(b, sizeof b, "%d", i + 1); cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(done || cur ? BG : MUTED); cv.setTextDatum(middle_center); cv.drawString(b, 56, y + 30);
    cv.setTextColor(cur ? INK : (done ? MUTED : INK)); cv.setTextDatum(middle_left); cv.drawString(STEPS[i].title, 86, y + 29);
  }
  // current step (right)
  int x = 520, w = W - x - 24;
  cv.fillRoundRect(x, 176, w, 300, 16, CARD);
  if (!job.active) {
    cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(job.complete ? GREEN : INK); cv.setTextDatum(middle_center);
    cv.drawString(job.complete ? "Job complete" : "Job not started", x + w / 2, 290);
    cv.setFont(&fonts::FreeSans12pt7b); cv.setTextColor(MUTED);
    cv.drawString(job.complete ? "Save the repair from the phone app later" : "Starting the job sends step 1 to the glasses", x + w / 2, 340);
    button(x, 500, w, 90, job.complete ? "Restart job" : "Start job on glasses", B_START, BLUE);
    return;
  }
  const Step& s = STEPS[job.i]; char b[64]; snprintf(b, sizeof b, "STEP %d OF %d", job.i + 1, NSTEPS);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(BLUE); cv.setTextDatum(top_left); cv.drawString(b, x + 24, 196);
  cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(INK); cv.drawString(s.title, x + 24, 226);
  for (int k = 0; k < 3; k++) { cv.fillCircle(x + 40, 302 + k * 52, 14, LINE); char nb2[3]; snprintf(nb2, sizeof nb2, "%d", k + 1);
    cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextColor(INK); cv.setTextDatum(middle_center); cv.drawString(nb2, x + 40, 303 + k * 52);
    cv.setFont(&fonts::FreeSans12pt7b); cv.setTextDatum(middle_left); cv.drawString(s.sub[k], x + 68, 302 + k * 52); }
  if (s.torque[0]) { snprintf(b, sizeof b, "Torque: %s", s.torque); cv.setTextColor(AMBER); cv.setTextDatum(top_left); cv.drawString(b, x + 24, 446); }
  int bw = (w - 24) / 2;
  button(x, 500, bw, 90, "Previous", B_PREV, job.i ? CARD : BG, job.i ? INK : LINE);
  button(x + bw + 24, 500, bw, 90, job.i == NSTEPS - 1 ? "Finish" : "Next step", B_NEXT, BLUE);
  cv.setFont(&fonts::FreeSans12pt7b); cv.setTextColor(MUTED); cv.setTextDatum(top_left);
  cv.drawString("Glasses button: 1 press = next - 2 presses = back - hold = hide panels", x, H - 120);
}

static void drawGlasses() {
  cv.setFont(&fonts::FreeSansBold18pt7b); cv.setTextColor(INK); cv.setTextDatum(top_left); cv.drawString("Glasses link (Wi-Fi hotspot)", 24, 96);
  int y = 150; char b[96];
  auto row = [&](const char* k, const char* v) { cv.setFont(&fonts::FreeSans18pt7b); cv.setTextColor(MUTED); cv.setTextDatum(top_left); cv.drawString(k, 24, y);
    cv.setTextColor(INK); cv.drawString(v, 330, y); y += 52; };
  row("Hotspot", wifiUp ? apName : "Wi-Fi failed to start (see README)");
  row("Password", AP_PASS);
  if (wifiUp) { snprintf(b, sizeof b, "%s : %u", WiFi.softAPIP().toString().c_str(), LINK_PORT); row("Address", b); }
  snprintf(b, sizeof b, "%d connected", connectedGlasses()); row("Glasses", b);
  row("Panels", panelsOn ? "Shown" : "Hidden (overlay only)");
  row("Panel lens", panelsRight ? "Right" : "Left");
  button(24, 480, 380, 90, panelsOn ? "Hide panels" : "Show panels", B_PANELS);
  button(430, 480, 380, 90, panelsRight ? "Panels to left lens" : "Panels to right lens", B_SIDE);
}

static void drawBus() {
  xSemaphoreTake(lock, portMAX_DELAY); uint32_t fr = D.frames, dm = D.dm1Count; xSemaphoreGive(lock);
  int y = 110; char b[64];
  auto row = [&](const char* k, const char* v) { cv.setFont(&fonts::FreeSans18pt7b); cv.setTextColor(MUTED); cv.setTextDatum(top_left); cv.drawString(k, 24, y);
    cv.setTextColor(INK); cv.drawString(v, 330, y); y += 52; };
  row("Mode", "Listen-only (never transmits)");
  row("Bus", demo ? "Demo traffic" : busState == BUS_OK ? (busBaud == 500000 ? "500 kbps" : "250 kbps") : busState == BUS_SCANNING ? "Scanning..." : "None found");
  row("Wiring", pinsSwapped ? "Port A (signal wires swapped)" : "Port A (G54 = RX)");
  snprintf(b, sizeof b, "%lu total  /  %lu per s", (unsigned long)fr, (unsigned long)fps); row("Frames", b);
  snprintf(b, sizeof b, "%lu", (unsigned long)dm); row("DM1 messages", b);
  button(24, 420, 380, 90, demo ? "Demo: ON" : "Demo mode", B_DEMO, demo ? AMBER : CARD, demo ? BG : INK);
  button(430, 420, 380, 90, "Rescan bus", B_SCAN);
}

static void onButton(int id) {
  switch (id) {
    case B_TAB0: case B_TAB1: case B_TAB2: case B_TAB3: tab = id - B_TAB0; break;
    case B_START: case B_JOBFROMCODE: jobStart(); tab = 1; break;
    case B_NEXT: jobNext(); break;
    case B_PREV: jobPrev(); break;
    case B_END: jobEnd(); break;
    case B_DEMO: demo = !demo; if (!demo) wantRescan = true; break;
    case B_SCAN: wantRescan = true; break;
    case B_PANELS: togglePanels(); break;
    case B_SIDE: panelsRight = !panelsRight; uiSeq++; break;
  }
}

// ======================================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(3);                                   // landscape
  W = M5.Display.width(); H = M5.Display.height();
  M5.Power.setExtOutput(true);                                 // 5 V to Grove (CAN Unit)
  expanderOutHigh(EXP1, 2);                                    // EXT5V_EN, in case
  cv.setPsram(true); cv.setColorDepth(16); cv.createSprite(W, H);
  lock = xSemaphoreCreateMutex();
  wifiSetup();
  xTaskCreatePinnedToCore(canTask, "can", 6144, nullptr, 3, nullptr, 1);
}

void loop() {
  M5.update();
  uint32_t now = millis();
  if (demo) demoTick();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) for (int i = nb - 1; i >= 0; i--) { const Btn& b = btns[i];
    if (t.x >= b.x && t.x < b.x + b.w && t.y >= b.y && t.y < b.y + b.h) { onButton(b.id); break; } }
  linkTick();
  static uint32_t nextDraw = 0;
  if (now >= nextDraw) {
    nextDraw = now + 250; nb = 0;
    cv.fillSprite(BG); drawHeader();
    if (tab == 0) drawDashboard(now); else if (tab == 1) drawJob(); else if (tab == 2) drawGlasses(); else drawBus();
    drawTabs();
    cv.pushSprite(0, 0);
  }
  delay(5);
}
