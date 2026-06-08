// =============================================================
// Settings-Cache (Firestore → ESP)
// =============================================================
// Hält System-Settings im RAM und persistiert sie in NVS.
// Wird für Tag/Nacht-Entscheidung, Mg-Ratio etc. gebraucht.
// =============================================================
#pragma once

#include <Preferences.h>
#include "config.h"
#include "firebase_sync.h"
#include "pumps.h"

namespace settings_cache {

bool autoDosing = false;          // Master-Schalter: ESP führt Plan autonom aus
int dosingsPerDay = 12;           // 2, 3, 4, 6, 8, 12 — Anzahl Dosier-Intervalle pro Tag
bool otaAutoUpdate = false;       // Master-Schalter: ESP installiert neuere GitHub-Releases automatisch
String healthcheckUrl = "";       // optionale URL (z.B. healthchecks.io) die bei jedem Heartbeat angepingt wird
bool usePhBasedKHDosing = false;
float phThresholdForKHNight = 8.0f;
int khNightStart = 19;
int khNightEnd = 7;
float magnesiumRatio = 50.0f;  // Prozent
unsigned long lastSyncMs = 0;

// ---------- NVS ----------
void loadFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  autoDosing = p.getBool("autoDose", false);
  dosingsPerDay = p.getInt("dosPerDay", 12);
  otaAutoUpdate = p.getBool("otaAuto", false);
  healthcheckUrl = p.getString("hcUrl", "");
  usePhBasedKHDosing = p.getBool("phMode", false);
  phThresholdForKHNight = p.getFloat("phThr", 8.0f);
  khNightStart = p.getInt("nightSt", 19);
  khNightEnd = p.getInt("nightEnd", 7);
  magnesiumRatio = p.getFloat("mgRatio", 50.0f);
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
  p.putString("hcUrl", healthcheckUrl);
  p.putBool("phMode", usePhBasedKHDosing);
  p.putFloat("phThr", phThresholdForKHNight);
  p.putInt("nightSt", khNightStart);
  p.putInt("nightEnd", khNightEnd);
  p.putFloat("mgRatio", magnesiumRatio);
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

  FirebaseJson doc;
  if (!firebase_sync::fetchSettings(doc)) return;

  bool changed = false;
  FirebaseJsonData v;
  if (doc.get(v, "fields/autoDosing/booleanValue") && v.success) {
    bool nv = (v.stringValue == "true");
    if (nv != autoDosing) { autoDosing = nv; changed = true; }
  }
  if (doc.get(v, "fields/dosingsPerDay/integerValue") && v.success) {
    int nv = (int)v.intValue;
    if (nv != dosingsPerDay && nv > 0 && nv <= 24 && (24 % nv == 0)) {
      dosingsPerDay = nv; changed = true;
    }
  }
  if (doc.get(v, "fields/otaAutoUpdate/booleanValue") && v.success) {
    bool nv = (v.stringValue == "true");
    if (nv != otaAutoUpdate) { otaAutoUpdate = nv; changed = true; }
  }
  if (doc.get(v, "fields/healthcheckUrl/stringValue") && v.success) {
    String nv = v.stringValue;
    if (nv != healthcheckUrl) { healthcheckUrl = nv; changed = true; }
  }
  if (doc.get(v, "fields/usePhBasedKHDosing/booleanValue") && v.success) {
    bool nv = (v.stringValue == "true");
    if (nv != usePhBasedKHDosing) { usePhBasedKHDosing = nv; changed = true; }
  }
  if (doc.get(v, "fields/phThresholdForKHNight/doubleValue") && v.success) {
    float nv = v.floatValue;
    if (nv != phThresholdForKHNight) { phThresholdForKHNight = nv; changed = true; }
  }
  if (doc.get(v, "fields/phThresholdForKHNight/integerValue") && v.success) {
    float nv = (float)v.intValue;
    if (nv != phThresholdForKHNight) { phThresholdForKHNight = nv; changed = true; }
  }
  if (doc.get(v, "fields/khNightStart/integerValue") && v.success) {
    int nv = (int)v.intValue;
    if (nv != khNightStart) { khNightStart = nv; changed = true; }
  }
  if (doc.get(v, "fields/khNightEnd/integerValue") && v.success) {
    int nv = (int)v.intValue;
    if (nv != khNightEnd) { khNightEnd = nv; changed = true; }
  }
  if (doc.get(v, "fields/magnesiumRatio/doubleValue") && v.success) {
    float nv = v.floatValue;
    if (nv != magnesiumRatio) { magnesiumRatio = nv; changed = true; }
  }
  if (doc.get(v, "fields/magnesiumRatio/integerValue") && v.success) {
    float nv = (float)v.intValue;
    if (nv != magnesiumRatio) { magnesiumRatio = nv; changed = true; }
  }
  // Schritt-Geschwindigkeit / -Beschleunigung
  if (doc.get(v, "fields/stepsPerSec/integerValue") && v.success) {
    uint32_t nv = (uint32_t)v.intValue;
    if (nv != pumps::stepsPerSec) { pumps::stepsPerSec = nv; changed = true; }
  }
  if (doc.get(v, "fields/accelStepsPerSec2/integerValue") && v.success) {
    uint32_t nv = (uint32_t)v.intValue;
    if (nv != pumps::accelPerSec2) { pumps::accelPerSec2 = nv; changed = true; }
  }
  // Anti-Drip Settings
  if (doc.get(v, "fields/enableAntiDrip/booleanValue") && v.success) {
    bool nv = (v.stringValue == "true");
    if (nv != pumps::antiDripEnabled) { pumps::antiDripEnabled = nv; changed = true; }
  }
  if (doc.get(v, "fields/antiDripML/integerValue") && v.success) {
    float nv = (float)v.intValue;
    if (nv != pumps::antiDripML) { pumps::antiDripML = nv; changed = true; }
  }
  if (doc.get(v, "fields/antiDripML/doubleValue") && v.success) {
    float nv = v.floatValue;
    if (nv != pumps::antiDripML) { pumps::antiDripML = nv; changed = true; }
  }
  if (doc.get(v, "fields/antiDripStepsPerSec/integerValue") && v.success) {
    uint32_t nv = (uint32_t)v.intValue;
    if (nv != pumps::antiDripStepsPerSec) { pumps::antiDripStepsPerSec = nv; changed = true; }
  }
  if (doc.get(v, "fields/antiDripAccelStepsPerSec2/integerValue") && v.success) {
    uint32_t nv = (uint32_t)v.intValue;
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
