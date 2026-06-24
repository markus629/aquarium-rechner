// =============================================================
// Settings-Cache (PocketBase → ESP)
// =============================================================
// Hält System-Settings im RAM und persistiert sie in NVS.
// Sync von PocketBase alle 2 Minuten + initiales Laden aus NVS.
//
// Felder:
//   autoDosing          - Master-Schalter Auto-Dosierung
//   otaAutoUpdate       - Auto-Install neuer GitHub-Releases
//   dosingsPerDay       - 2/3/4/6/8/12 (Anzahl Dosier-Intervalle/Tag)
//   usePhBasedKHDosing  - Tag/Nacht-Schaltung via pH (statt Uhrzeit)
//   phThresholdForKHNight, khNightStart, khNightEnd
//   magnesiumRatio      - Mg-Dosis in % der Ca-Dosis
//   Stepper-Konfig wird in pumps:: gehalten, hier nur gesynct
// =============================================================
#pragma once

#include <Preferences.h>
#include "config.h"
#include "pb_sync.h"
#include "pumps.h"

namespace settings_cache {

bool autoDosing = false;          // Master-Schalter: ESP führt Plan autonom aus
int dosingsPerDay = 12;           // 2, 3, 4, 6, 8, 12 — Anzahl Dosier-Intervalle pro Tag
bool otaAutoUpdate = false;       // Master-Schalter: ESP installiert neuere GitHub-Releases automatisch
bool usePhBasedKHDosing = false;
float phThresholdForKHNight = 8.0f;
int khNightStart = 19;
int khNightEnd = 7;
float magnesiumRatio = 50.0f;  // Prozent
// Dosier-Zeitfenster je Nährstoff-Gruppe (Stunden 0–24). 0–24 = ganzer Tag.
// NO₃-Fenster steuert N + C, PO₄-Fenster steuert P + Lanthan. (nur Nährstoff-Rolle)
int no3FromHour = 0, no3ToHour = 24;
int po4FromHour = 0, po4ToHour = 24;
unsigned long lastSyncMs = 0;

// ---------- NVS ----------
void loadFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  autoDosing = p.getBool("autoDose", false);
  dosingsPerDay = p.getInt("dosPerDay", 12);
  otaAutoUpdate = p.getBool("otaAuto", false);
  usePhBasedKHDosing = p.getBool("phMode", false);
  phThresholdForKHNight = p.getFloat("phThr", 8.0f);
  khNightStart = p.getInt("nightSt", 19);
  khNightEnd = p.getInt("nightEnd", 7);
  magnesiumRatio = p.getFloat("mgRatio", 50.0f);
  no3FromHour = p.getInt("no3From", 0);  no3ToHour = p.getInt("no3To", 24);
  po4FromHour = p.getInt("po4From", 0);  po4ToHour = p.getInt("po4To", 24);
  pumps::stepsPerSec  = (uint32_t)p.getULong("stepsHz", 400);
  pumps::accelPerSec2 = (uint32_t)p.getULong("accelHz", 200);
  pumps::antiDripEnabled = p.getBool("adEn", true);
  pumps::antiDripML = p.getFloat("adML", 0.015f);
  pumps::antiDripStepsPerSec = (uint32_t)p.getULong("adHz", 400);
  pumps::antiDripAccelPerSec2 = (uint32_t)p.getULong("adAcc", 1000);
  p.end();
}

void saveToNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putBool("autoDose", autoDosing);
  p.putInt("dosPerDay", dosingsPerDay);
  p.putBool("otaAuto", otaAutoUpdate);
  p.putBool("phMode", usePhBasedKHDosing);
  p.putFloat("phThr", phThresholdForKHNight);
  p.putInt("nightSt", khNightStart);
  p.putInt("nightEnd", khNightEnd);
  p.putFloat("mgRatio", magnesiumRatio);
  p.putInt("no3From", no3FromHour);  p.putInt("no3To", no3ToHour);
  p.putInt("po4From", po4FromHour);  p.putInt("po4To", po4ToHour);
  p.putULong("stepsHz", (unsigned long)pumps::stepsPerSec);
  p.putULong("accelHz", (unsigned long)pumps::accelPerSec2);
  p.putBool("adEn", pumps::antiDripEnabled);
  p.putFloat("adML", pumps::antiDripML);
  p.putULong("adHz", (unsigned long)pumps::antiDripStepsPerSec);
  p.putULong("adAcc", (unsigned long)pumps::antiDripAccelPerSec2);
  p.end();
}

void begin() { loadFromNVS(); }

// ---------- Cloud-Sync ----------
// Alle 2 Min Settings holen (klein, billig)
void sync() {
  unsigned long now = millis();
  if (lastSyncMs != 0 && now - lastSyncMs < 2UL * 60 * 1000) return;
  lastSyncMs = now;

  JsonDocument doc;
  if (!pb_sync::fetchSettings(doc)) return;

  bool changed = false;
  // PocketBase liefert flaches JSON; is<float>() ist auch für Ganzzahlen true.
  if (doc["autoDosing"].is<bool>()) {
    bool nv = doc["autoDosing"].as<bool>();
    if (nv != autoDosing) { autoDosing = nv; changed = true; }
  }
  if (doc["dosingsPerDay"].is<float>()) {
    int nv = (int)doc["dosingsPerDay"].as<float>();
    if (nv != dosingsPerDay && nv > 0 && nv <= 24 && (24 % nv == 0)) {
      dosingsPerDay = nv; changed = true;
    }
  }
  if (doc["otaAutoUpdate"].is<bool>()) {
    bool nv = doc["otaAutoUpdate"].as<bool>();
    if (nv != otaAutoUpdate) { otaAutoUpdate = nv; changed = true; }
  }
  if (doc["usePhBasedKHDosing"].is<bool>()) {
    bool nv = doc["usePhBasedKHDosing"].as<bool>();
    if (nv != usePhBasedKHDosing) { usePhBasedKHDosing = nv; changed = true; }
  }
  if (doc["phThresholdForKHNight"].is<float>()) {
    float nv = doc["phThresholdForKHNight"].as<float>();
    if (nv != phThresholdForKHNight) { phThresholdForKHNight = nv; changed = true; }
  }
  if (doc["khNightStart"].is<float>()) {
    int nv = (int)doc["khNightStart"].as<float>();
    if (nv != khNightStart) { khNightStart = nv; changed = true; }
  }
  if (doc["khNightEnd"].is<float>()) {
    int nv = (int)doc["khNightEnd"].as<float>();
    if (nv != khNightEnd) { khNightEnd = nv; changed = true; }
  }
  if (doc["magnesiumRatio"].is<float>()) {
    float nv = doc["magnesiumRatio"].as<float>();
    if (nv != magnesiumRatio) { magnesiumRatio = nv; changed = true; }
  }
  // Dosier-Zeitfenster (Nährstoff-Rolle)
  if (doc["no3FromHour"].is<float>()) { int nv=(int)doc["no3FromHour"].as<float>(); if(nv!=no3FromHour){no3FromHour=nv;changed=true;} }
  if (doc["no3ToHour"].is<float>())   { int nv=(int)doc["no3ToHour"].as<float>();   if(nv!=no3ToHour){no3ToHour=nv;changed=true;} }
  if (doc["po4FromHour"].is<float>()) { int nv=(int)doc["po4FromHour"].as<float>(); if(nv!=po4FromHour){po4FromHour=nv;changed=true;} }
  if (doc["po4ToHour"].is<float>())   { int nv=(int)doc["po4ToHour"].as<float>();   if(nv!=po4ToHour){po4ToHour=nv;changed=true;} }
  // Schritt-Geschwindigkeit / -Beschleunigung
  if (doc["stepsPerSec"].is<float>()) {
    uint32_t nv = (uint32_t)doc["stepsPerSec"].as<float>();
    if (nv > 0 && nv != pumps::stepsPerSec) { pumps::stepsPerSec = nv; changed = true; }
  }
  if (doc["accelStepsPerSec2"].is<float>()) {
    uint32_t nv = (uint32_t)doc["accelStepsPerSec2"].as<float>();
    if (nv != pumps::accelPerSec2) { pumps::accelPerSec2 = nv; changed = true; }
  }
  // Anti-Drip Settings
  if (doc["enableAntiDrip"].is<bool>()) {
    bool nv = doc["enableAntiDrip"].as<bool>();
    if (nv != pumps::antiDripEnabled) { pumps::antiDripEnabled = nv; changed = true; }
  }
  if (doc["antiDripML"].is<float>()) {
    float nv = doc["antiDripML"].as<float>();
    if (nv != pumps::antiDripML) { pumps::antiDripML = nv; changed = true; }
  }
  if (doc["antiDripStepsPerSec"].is<float>()) {
    uint32_t nv = (uint32_t)doc["antiDripStepsPerSec"].as<float>();
    if (nv > 0 && nv != pumps::antiDripStepsPerSec) { pumps::antiDripStepsPerSec = nv; changed = true; }
  }
  if (doc["antiDripAccelStepsPerSec2"].is<float>()) {
    uint32_t nv = (uint32_t)doc["antiDripAccelStepsPerSec2"].as<float>();
    if (nv != pumps::antiDripAccelPerSec2) { pumps::antiDripAccelPerSec2 = nv; changed = true; }
  }
  if (changed) {
    saveToNVS();
    Serial.printf("[Settings] aktualisiert: autoDose=%d, ph-Mode=%d, ph-Thr=%.2f, Nacht %d-%d, Mg-Ratio=%.1f%%, Speed=%u Hz, Accel=%u Hz/s\n",
                  autoDosing, usePhBasedKHDosing, phThresholdForKHNight, khNightStart, khNightEnd, magnesiumRatio,
                  (unsigned)pumps::stepsPerSec, (unsigned)pumps::accelPerSec2);
  }
}

// Intervall in Stunden = 24 / dosingsPerDay. Bei ungültigem Wert: 2 (= 12/Tag).
int intervalHours() {
  if (dosingsPerDay <= 0 || dosingsPerDay > 24 || (24 % dosingsPerDay != 0)) return 2;
  return 24 / dosingsPerDay;
}

// Dosier-Slots (ganze Stunden) aus Zeitfenster + Anzahl Dosen → slots[], gibt Anzahl.
// MUSS identisch zur Web-Funktion doseSlotHours() (nutrient/index.html) sein!
// Volltag (0–24 / from==to): gleichmäßig über 24 h ab Stunde 0. Fenster: erste am
// Start, letzte am Ende; n=1 → nur Start. Ganzzahl-Rundung floor((2a+b)/(2b)).
int doseSlotHours(int fromH, int toH, int n, int *slots) {
  if (n < 1) n = 1;
  int f = ((fromH % 24) + 24) % 24;
  int t = toH;
  bool full = (f == 0 && (t >= 24 || t == 0)) || ((((t % 24) + 24) % 24) == f);
  bool used[24] = { false };
  if (full) {
    for (int i = 0; i < n; i++) used[ ((2*i*24 + n) / (2*n)) % 24 ] = true;
  } else {
    int span = (((t - f) % 24) + 24) % 24;   // 1..23
    if (n == 1) used[f] = true;
    else for (int i = 0; i < n; i++) used[ (f + ((2*i*span + (n-1)) / (2*(n-1)))) % 24 ] = true;
  }
  int cnt = 0;
  for (int h = 0; h < 24; h++) if (used[h]) slots[cnt++] = h;
  return cnt;
}
bool hourIsDoseSlot(int hour, int fromH, int toH, int n) {
  int slots[24]; int c = doseSlotHours(fromH, toH, n, slots);
  for (int i = 0; i < c; i++) if (slots[i] == hour) return true;
  return false;
}

// ---------- Tag/Nacht-Entscheidung ----------
// Returns true wenn Nacht-Pumpe verwendet werden soll.
bool isKHNightNow(float currentPh) {
  if (usePhBasedKHDosing) {
    if (isnan(currentPh)) return false;  // ohne pH-Wert: konservativ Tag-Pumpe
    return currentPh < phThresholdForKHNight;
  }
  // Zeit-basiert: Stunde aus lokaler Zeit
  time_t now; time(&now);
  if (now < 1700000000) return false;  // Zeit noch nicht synced
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  int hour = timeinfo.tm_hour;
  // Wrap-around: Nacht-Start 19, Nacht-Ende 7 → Nacht=19..23 ODER 0..6
  if (khNightStart < khNightEnd) {
    return hour >= khNightStart && hour < khNightEnd;
  } else {
    return hour >= khNightStart || hour < khNightEnd;
  }
}

} // namespace settings_cache
