// TraceView Bridge — J1939 decoder (no hardware dependencies, unit-tested on a PC)
// Listen-only: this code only reads frames. It never builds a frame to send.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

namespace j1939 {

// ---- PGNs we use ----
enum : uint32_t {
  PGN_TP_CM = 60416,  // 0xEC00 transport protocol, connection management (BAM)
  PGN_TP_DT = 60160,  // 0xEB00 transport protocol, data transfer
  PGN_DM1   = 65226,  // 0xFECA active diagnostic trouble codes
  PGN_EEC1  = 61444,  // 0xF004 engine speed
  PGN_VEP1  = 65271,  // 0xFEF7 battery / keyswitch potential
  PGN_ET1   = 65262,  // 0xFEEE engine coolant temperature
  PGN_EFLP1 = 65263,  // 0xFEEF engine oil pressure
  PGN_CCVS1 = 65265,  // 0xFEF1 wheel-based vehicle speed
  PGN_VI    = 65260,  // 0xFEEC vehicle identification (VIN)
};

struct Id { uint8_t prio, sa, da; uint32_t pgn; };

inline Id parseId(uint32_t id) {
  Id r;
  r.prio = (id >> 26) & 7;
  uint8_t dp = (id >> 24) & 3;           // EDP + DP
  uint8_t pf = (id >> 16) & 0xFF;
  uint8_t ps = (id >> 8) & 0xFF;
  r.sa = id & 0xFF;
  if (pf < 240) { r.pgn = ((uint32_t)dp << 16) | ((uint32_t)pf << 8); r.da = ps; }   // PDU1: PS is destination
  else          { r.pgn = ((uint32_t)dp << 16) | ((uint32_t)pf << 8) | ps; r.da = 0xFF; } // PDU2: broadcast
  return r;
}

struct Dtc { uint8_t sa; uint32_t spn; uint8_t fmi; uint8_t oc; uint32_t seenMs; };
struct Lamps { uint8_t mil, rsl, awl, pl; };   // 1 = on

static const int MAX_DTC = 32;
static const int MAX_TP  = 6;
static const int TP_BUF  = 256;

struct Live {
  float rpm = -1, batV = -1, keyV = -1, coolF = -999, oilPsi = -1, mph = -1;
  uint32_t tRpm = 0, tBat = 0, tCool = 0, tOil = 0, tMph = 0;
  char vin[21] = {0};
};

struct Decoder {
  Dtc dtc[MAX_DTC]; int nDtc = 0;
  Lamps lampBySa[256];
  uint32_t dm1SeenBySa[256];
  Live live;
  uint32_t frames = 0, dm1Count = 0, changeSeq = 0;

  struct Tp { bool active; uint8_t sa; uint32_t pgn; uint16_t size; uint8_t packets, next; uint8_t buf[TP_BUF]; uint32_t startMs; };
  Tp tp[MAX_TP];

  Decoder() { memset(lampBySa, 0, sizeof lampBySa); memset(dm1SeenBySa, 0, sizeof dm1SeenBySa); memset(tp, 0, sizeof tp); }

  static inline bool na8(uint8_t v) { return v >= 0xFB; }        // 0xFB..0xFF = reserved / not available
  static inline bool na16(uint16_t v) { return v >= 0xFB00; }

  // ---- DM1 payload: [lamp][flash][DTC 4 bytes]... ----
  void onDm1(uint8_t sa, const uint8_t* d, int len, uint32_t now) {
    dm1Count++;
    if (len < 2) return;
    Lamps L;
    L.mil = (d[0] >> 6) & 3; L.rsl = (d[0] >> 4) & 3; L.awl = (d[0] >> 2) & 3; L.pl = d[0] & 3;
    L.mil = L.mil == 1; L.rsl = L.rsl == 1; L.awl = L.awl == 1; L.pl = L.pl == 1;
    lampBySa[sa] = L; dm1SeenBySa[sa] = now ? now : 1;
    // remove this source's old codes, then add the new set
    int before = nDtc; uint32_t sig0 = signature();
    int w = 0; for (int i = 0; i < nDtc; i++) if (dtc[i].sa != sa) dtc[w++] = dtc[i]; nDtc = w;
    for (int o = 2; o + 4 <= len; o += 4) {
      uint32_t spn = d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)(d[o + 2] & 0xE0) << 11);
      uint8_t fmi = d[o + 2] & 0x1F, oc = d[o + 3] & 0x7F;
      if (spn == 0 && fmi == 0) continue;                 // "no active codes" placeholder
      if (spn == 0x7FFFF) continue;                       // not available
      if (nDtc < MAX_DTC) dtc[nDtc++] = Dtc{sa, spn, fmi, oc, now};
    }
    if (nDtc != before || signature() != sig0) changeSeq++;
  }

  uint32_t signature() const { uint32_t s = 2166136261u; for (int i = 0; i < nDtc; i++) { s = (s ^ dtc[i].sa) * 16777619u; s = (s ^ dtc[i].spn) * 16777619u; s = (s ^ dtc[i].fmi) * 16777619u; } return s; }

  // drop codes from modules that stopped sending DM1 (DM1 repeats every second)
  void expire(uint32_t now, uint32_t maxAgeMs = 5000) {
    int w = 0; bool ch = false;
    for (int i = 0; i < nDtc; i++) { if (now - dm1SeenBySa[dtc[i].sa] <= maxAgeMs) dtc[w++] = dtc[i]; else ch = true; }
    nDtc = w; if (ch) changeSeq++;
  }

  Lamps lamps(uint32_t now, uint32_t maxAgeMs = 5000) const {
    Lamps r{0, 0, 0, 0};
    for (int sa = 0; sa < 256; sa++) if (dm1SeenBySa[sa] && now - dm1SeenBySa[sa] <= maxAgeMs) {
      r.mil |= lampBySa[sa].mil; r.rsl |= lampBySa[sa].rsl; r.awl |= lampBySa[sa].awl; r.pl |= lampBySa[sa].pl; }
    return r;
  }

  void onPgn(uint8_t sa, uint32_t pgn, const uint8_t* d, int len, uint32_t now) {
    switch (pgn) {
      case PGN_DM1: onDm1(sa, d, len, now); break;
      case PGN_EEC1: if (len >= 5) { uint16_t v = d[3] | (d[4] << 8); if (!na16(v) && sa == 0) { live.rpm = v * 0.125f; live.tRpm = now; } } break;
      case PGN_VEP1: if (len >= 8) { uint16_t b = d[4] | (d[5] << 8), k = d[6] | (d[7] << 8);
                       if (!na16(b)) { live.batV = b * 0.05f; live.tBat = now; } if (!na16(k)) live.keyV = k * 0.05f; } break;
      case PGN_ET1:  if (len >= 1 && !na8(d[0]) && sa == 0) { live.coolF = (d[0] - 40) * 1.8f + 32; live.tCool = now; } break;
      case PGN_EFLP1:if (len >= 4 && !na8(d[3]) && sa == 0) { live.oilPsi = d[3] * 4 * 0.145038f; live.tOil = now; } break;
      case PGN_CCVS1:if (len >= 3) { uint16_t v = d[1] | (d[2] << 8); if (!na16(v)) { live.mph = v / 256.0f * 0.621371f; live.tMph = now; } } break;
      case PGN_VI: { char tmp[21] = {0}; int n = 0;
                     for (int i = 0; i < len && n < 20; i++) { char c = (char)d[i]; if (c == '*') break; if (c >= '0' && c <= 'Z') tmp[n++] = c; }
                     if (n == 17) memcpy(live.vin, tmp, 18); } break;
    }
  }

  // ---- feed one CAN frame (29-bit id) ----
  void onFrame(uint32_t id, bool extended, const uint8_t* d, int len, uint32_t now) {
    if (!extended) return;               // J1939 uses 29-bit ids
    frames++;
    Id x = parseId(id);
    if (x.pgn == PGN_TP_CM && len >= 8) {
      uint8_t ctrl = d[0];
      if (ctrl == 32 && x.da == 0xFF) {  // BAM (broadcast) only; we never answer RTS/CTS
        uint32_t pgn = d[5] | ((uint32_t)d[6] << 8) | ((uint32_t)d[7] << 16);
        uint16_t size = d[1] | (d[2] << 8); uint8_t packets = d[3];
        if (size > TP_BUF || packets == 0) return;
        Tp* t = slot(x.sa);
        t->active = true; t->sa = x.sa; t->pgn = pgn; t->size = size; t->packets = packets; t->next = 1; t->startMs = now;
        memset(t->buf, 0xFF, sizeof t->buf);
      } else if (ctrl == 255) { Tp* t = find(x.sa); if (t) t->active = false; }  // abort
      return;
    }
    if (x.pgn == PGN_TP_DT && len >= 1) {
      if (x.da != 0xFF) return;
      Tp* t = find(x.sa); if (!t) return;
      uint8_t seq = d[0];
      if (seq != t->next) { t->active = false; return; }   // lost a packet: drop, the next broadcast repeats it
      int off = (seq - 1) * 7;
      for (int i = 1; i < len && i < 8; i++) if (off + i - 1 < TP_BUF) t->buf[off + i - 1] = d[i];
      t->next++;
      if (seq == t->packets) { t->active = false; onPgn(t->sa, t->pgn, t->buf, t->size, now); }
      return;
    }
    onPgn(x.sa, x.pgn, d, len, now);
  }

  Tp* find(uint8_t sa) { for (auto& t : tp) if (t.active && t.sa == sa) return &t; return nullptr; }
  Tp* slot(uint8_t sa) {
    Tp* f = find(sa); if (f) return f;
    for (auto& t : tp) if (!t.active) return &t;
    Tp* old = &tp[0]; for (auto& t : tp) if (t.startMs < old->startMs) old = &t; return old;
  }

  // ---- JSON for the phone (one line each) ----
  int jsonCodes(char* out, int cap, uint32_t now) const {
    Lamps L = lamps(now);
    int n = snprintf(out, cap, "{\"t\":\"dm1\",\"lamps\":{\"mil\":%d,\"rsl\":%d,\"awl\":%d,\"pl\":%d},\"codes\":[", L.mil, L.rsl, L.awl, L.pl);
    for (int i = 0; i < nDtc && n < cap - 48; i++)
      n += snprintf(out + n, cap - n, "%s{\"sa\":%u,\"spn\":%lu,\"fmi\":%u,\"oc\":%u}", i ? "," : "", dtc[i].sa, (unsigned long)dtc[i].spn, dtc[i].fmi, dtc[i].oc);
    n += snprintf(out + n, cap - n, "]}\n");
    return n;
  }
  static void fmtF(char* b, int cap, float v, uint32_t t, uint32_t now, const char* fmt) { if (t && now - t < 5000) snprintf(b, cap, fmt, v); else snprintf(b, cap, "null"); }
  int jsonLive(char* out, int cap, uint32_t now) const {
    char rpm[16], bat[16], cool[16], oil[16], mph[16];
    fmtF(rpm, 16, live.rpm, live.tRpm, now, "%.0f"); fmtF(bat, 16, live.batV, live.tBat, now, "%.2f");
    fmtF(cool, 16, live.coolF, live.tCool, now, "%.0f"); fmtF(oil, 16, live.oilPsi, live.tOil, now, "%.0f"); fmtF(mph, 16, live.mph, live.tMph, now, "%.0f");
    return snprintf(out, cap, "{\"t\":\"live\",\"rpm\":%s,\"bat\":%s,\"cool\":%s,\"oil\":%s,\"mph\":%s,\"vin\":\"%s\"}\n", rpm, bat, cool, oil, mph, live.vin);
  }
};

// ---- names shown on the CoreS3 screen ----
inline const char* saName(uint8_t sa) {
  switch (sa) {
    case 0: return "Engine"; case 1: return "Engine #2"; case 3: return "Transmission"; case 11: return "Brakes (ABS)";
    case 15: return "Retarder"; case 23: return "Instrument cluster"; case 33: return "Body controller";
    case 49: return "Cab controller"; case 61: return "Aftertreatment"; default: return nullptr;
  }
}
inline const char* fmiText(uint8_t f) {
  static const char* t[] = {
    "Above normal, most severe", "Below normal, most severe", "Erratic / intermittent", "Voltage high / shorted high",
    "Voltage low / shorted low", "Current low / open circuit", "Current high / grounded", "Mechanical not responding",
    "Abnormal frequency", "Abnormal update rate", "Abnormal rate of change", "Root cause unknown",
    "Bad device or component", "Out of calibration", "Special instructions", "Above normal, least severe",
    "Above normal, moderate", "Below normal, least severe", "Below normal, moderate", "Network data error",
    "Data drifted high", "Data drifted low"};
  if (f < sizeof(t) / sizeof(t[0])) return t[f];
  if (f == 31) return "Condition exists";
  return "Reserved";
}
inline const char* spnName(uint32_t spn) {
  switch (spn) {
    case 168: return "Battery potential"; case 158: return "Keyswitch battery potential"; case 110: return "Coolant temperature";
    case 100: return "Oil pressure"; case 190: return "Engine speed"; case 84: return "Vehicle speed"; case 94: return "Fuel delivery pressure";
    case 3251: return "DPF differential pressure"; case 3226: return "Aftertreatment outlet NOx"; case 4364: return "SCR conversion efficiency";
    case 5246: return "SCR inducement severity"; case 1569: return "Engine derate"; case 639: return "J1939 network #1";
    default: return nullptr;
  }
}

} // namespace j1939
