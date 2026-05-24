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
  usePhBasedKHDosing = p.getBool("phMode", false);
  phThresholdForKHNight = p.getFloat("phThr", 8.0f);
  khNightStart = p.getInt("nightSt", 19);
  khNightEnd = p.getInt("nightEnd", 7);
  magnesiumRatio = p.getFloat("mgRatio", 50.0f);
  pumps::speedML = p.getFloat("speedML", DEFAULT_SPEED_ML);
  pumps::accelML = p.getFloat("accelML", DEFAULT_ACCELERATION_ML);
  p.end();
}

void saveToNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putBool("phMode", usePhBasedKHDosing);
  p.putFloat("phThr", phThresholdForKHNight);
  p.putInt("nightSt", khNightStart);
  p.putInt("nightEnd", khNightEnd);
  p.putFloat("mgRatio", magnesiumRatio);
  p.putFloat("speedML", pumps::speedML);
  p.putFloat("accelML", pumps::accelML);
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
  if (doc.get(v, "fields/usePhBasedKHDosing/booleanValue") && v.success) {
    bool nv = (v.stringValue == "true");
    if (nv != usePhBasedKHDosing) { usePhBasedKHDosing = nv; changed = true; }
  }
  if (doc.get(v, "fields/phThresholdForKHNight/doubleValue") && v.success) {
    float nv = v.floatValue;
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
  // Speed / Acceleration (kommen aus Pump-Settings)
  if (doc.get(v, "fields/speedML/doubleValue") && v.success) {
    float nv = v.floatValue;
    if (nv != pumps::speedML) { pumps::speedML = nv; changed = true; }
  }
  if (doc.get(v, "fields/accelerationML/doubleValue") && v.success) {
    float nv = v.floatValue;
    if (nv != pumps::accelML) { pumps::accelML = nv; changed = true; }
  }
  if (changed) {
    saveToNVS();
    Serial.printf("[Settings] aktualisiert: ph-Mode=%d, ph-Thr=%.2f, Nacht %d-%d, Mg-Ratio=%.1f%%, Speed=%.2f ml/min, Accel=%.2f ml/min²\n",
                  usePhBasedKHDosing, phThresholdForKHNight, khNightStart, khNightEnd, magnesiumRatio,
                  pumps::speedML, pumps::accelML);
  }
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
