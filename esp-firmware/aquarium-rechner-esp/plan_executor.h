// =============================================================
// Plan-Executor + Command-Dispatcher
// =============================================================
// Portiert die bewährte Logik aus 088_sketch_sep3a_01.ino:
//
// - Trigger-Fenster: NUR in Minute 10-15 jeder GERADEN Stunde
//   (0:10, 2:10, 4:10, ..., 22:10) — deterministisches 2-h-Raster
// - Pro Stunde dedupliziert via lastDosageTimeCache[] pro Typ
// - Pro Typ wird geprüft: gibt es eine spezifische Plan-Dose in dieser
//   Stunde (AUSGLEICH-Phase) oder nicht (ERHALTUNG-Phase)
// - Alle anwendbaren Doses (KH + Ca + Mg) werden in EINE Sequenz
//   gepackt → laufen sequentiell ab via Queue
// - KH-Tag/Nacht-Entscheidung zur Laufzeit (pH-basiert oder Uhrzeit)
// - KH-Nacht: ml × 0.5 (Konzentrat ist doppelt konzentriert)
//
// Cached Plan + Settings + Kalibrierung + Maintenance-Timestamps im NVS.
// =============================================================
#pragma once

#include <Preferences.h>
#include "config.h"
#include "firebase_sync.h"
#include "pumps.h"
#include "ph_sensor.h"
#include "settings_cache.h"

namespace plan_executor {

unsigned long lastCommandPollMs = 0;
unsigned long lastPumpConfigSyncMs = 0;
unsigned long lastPlanFetchMs = 0;
unsigned long lastSequenceCheckMs = 0;
time_t lastPhSampleAt = 0;  // letzter pH-Sample-Zeitpunkt (für 2h-Raster)

// ---------- Statistiken (für Heartbeat) ----------
unsigned long dosesTotal = 0;
unsigned long dosesFailedTotal = 0;
// Ring-Buffer der letzten 100 Dose-Timestamps für "letzte 24h"-Statistik
static const int DOSE_TIMES_SIZE = 100;
time_t doseTimestamps[DOSE_TIMES_SIZE] = {0};
bool doseSuccess[DOSE_TIMES_SIZE] = {0};
int doseTimesHead = 0;

void recordDose(time_t ts, bool success) {
  doseTimestamps[doseTimesHead] = ts;
  doseSuccess[doseTimesHead] = success;
  doseTimesHead = (doseTimesHead + 1) % DOSE_TIMES_SIZE;
  dosesTotal++;
  if (!success) dosesFailedTotal++;
}

void countDosesLast24h(int &okCount, int &failCount) {
  okCount = 0; failCount = 0;
  time_t now; time(&now);
  if (now < 1700000000) return;
  for (int i = 0; i < DOSE_TIMES_SIZE; i++) {
    if (doseTimestamps[i] > 0 && now - doseTimestamps[i] <= 24L * 3600) {
      if (doseSuccess[i]) okCount++;
      else failCount++;
    }
  }
}
const unsigned long PLAN_FETCH_INTERVAL_MS = 30UL * 1000;       // 30s = Quasi-Push
const unsigned long SEQUENCE_CHECK_INTERVAL_MS = 30UL * 1000;   // jede 30s in Trigger-Fenster gucken

// Dosage-Typen (für lastDosageTimeCache + dosings)
enum DosageType {
  DT_KH_DAY = 0,
  DT_KH_NIGHT = 1,
  DT_CA = 2,
  DT_MG = 3,
  DT_COUNT = 4
};
const char* DT_LABELS[DT_COUNT] = { "kh-day", "kh-night", "ca", "mg" };
time_t lastDosageTimeCache[DT_COUNT] = { 0, 0, 0, 0 };

// ---------- Aktive Dose / Command ----------
struct ActiveCmd {
  String cmdId;       // Web-Command ID (leer wenn Plan-Dose)
  String action;
  int pump = -1;
  float ml = NAN;
  long steps = 0;
  unsigned long startMs = 0;
  bool active = false;
  bool fromPlan = false;
  DosageType dosageType = DT_KH_DAY;
};
ActiveCmd current;
String lastProcessedCmdId = "";  // verhindert Doppelausführung gleicher Web-Command

// ---------- Dose-Queue (für Sequenz: KH → Ca → Mg) ----------
struct QueuedDose {
  int pump;
  float ml;
  DosageType type;
};
static const int MAX_QUEUE = 4;
QueuedDose doseQueue[MAX_QUEUE];
int queueHead = 0, queueTail = 0;

bool queueEmpty() { return queueHead == queueTail; }
bool queueFull()  { return ((queueTail + 1) % MAX_QUEUE) == queueHead; }
void enqueueDose(int pump, float ml, DosageType type) {
  if (queueFull()) { Serial.println("[Queue] FULL"); return; }
  doseQueue[queueTail] = { pump, ml, type };
  queueTail = (queueTail + 1) % MAX_QUEUE;
  Serial.printf("[Queue] +%s pump=%d ml=%.2f\n", DT_LABELS[type], pump, ml);
}
bool dequeueDose(QueuedDose &out) {
  if (queueEmpty()) return false;
  out = doseQueue[queueHead];
  queueHead = (queueHead + 1) % MAX_QUEUE;
  return true;
}

// ---------- Plan-Cache (NVS) ----------
String cachedPlanJson = "";
unsigned long cachedPlanTimeMs = 0;

void loadFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  cachedPlanJson = p.getString("planJson", "");
  cachedPlanTimeMs = (unsigned long)p.getULong("planTime", 0);
  for (int i = 0; i < DT_COUNT; i++) {
    String key = "lastD" + String(i);
    lastDosageTimeCache[i] = (time_t)p.getULong(key.c_str(), 0);
  }
  p.end();
  if (cachedPlanJson.length() > 0)
    Serial.printf("[Plan] Cache aus NVS: %d Zeichen\n", cachedPlanJson.length());
}

void saveLastDosageTime(DosageType t) {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  String key = "lastD" + String((int)t);
  p.putULong(key.c_str(), (unsigned long)lastDosageTimeCache[t]);
  p.end();
}

void savePlanCacheToNVS(const String &json) {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putString("planJson", json);
  p.putULong("planTime", (unsigned long)(millis()));
  p.end();
}

// ---------- Pump-Configs alle 5 Min syncen ----------
void syncPumpConfigs() {
  unsigned long now = millis();
  if (lastPumpConfigSyncMs != 0 && now - lastPumpConfigSyncMs < 5UL * 60 * 1000) return;
  lastPumpConfigSyncMs = now;
  for (int i = 0; i < pumps::NUM_PUMPS; i++) {
    float v;
    if (firebase_sync::fetchPumpStepsPerML(i, v)) {
      if (v != pumps::stepsPerML[i]) {
        pumps::setStepsPerML(i, v);
        Serial.printf("[PumpConfig] Pumpe %d: %.2f Schritte/ml\n", i, v);
      }
    }
  }
}

// ---------- Plan alle 30s syncen (Quasi-Push) ----------
void syncPlan() {
  if (!firebase_sync::isReady()) return;
  unsigned long now = millis();
  if (lastPlanFetchMs != 0 && now - lastPlanFetchMs < PLAN_FETCH_INTERVAL_MS) return;
  lastPlanFetchMs = now;

  FirebaseJson doc;
  if (!firebase_sync::fetchPlan(doc)) return;
  String json;
  doc.toString(json);
  if (json.length() > 0 && json != cachedPlanJson) {
    cachedPlanJson = json;
    cachedPlanTimeMs = now;
    savePlanCacheToNVS(json);
    Serial.printf("[Plan] aktualisiert (%d Zeichen)\n", json.length());
  }
}

bool isPlanCacheStale() {
  if (cachedPlanJson.length() == 0) return true;
  return (millis() - cachedPlanTimeMs) > PLAN_CACHE_TTL_MS;
}

// ---------- Dose-Sequenz aus Queue starten ----------
bool startNextQueuedDose() {
  if (current.active) return false;
  QueuedDose next;
  if (!dequeueDose(next)) return false;
  if (!pumps::runMl(next.pump, next.ml)) {
    Serial.printf("[Plan] Pumpe %d nicht startbar — überspringe\n", next.pump);
    return startNextQueuedDose();  // try next
  }
  current = {};
  current.action = "dose";
  current.pump = next.pump;
  current.ml = next.ml;
  current.startMs = millis();
  current.active = true;
  current.fromPlan = true;
  current.dosageType = next.type;
  Serial.printf("[Plan] DOSE %s pump=%d ml=%.2f\n", DT_LABELS[next.type], next.pump, next.ml);
  return true;
}

// ---------- pH-Kalibrierung (Command-Handler) ----------
// Sampelt 10 Sekunden lang, mittelt die Voltage, speichert für gewählten pH-Wert.
struct PhCalibrationCmd {
  bool active = false;
  String cmdId;
  float targetPh = 0;
  unsigned long startMs = 0;
  float voltageSum = 0;
  int sampleCount = 0;
};
PhCalibrationCmd phCal;

bool startPhCalibration(const String &cmdId, float targetPh) {
  if (phCal.active) {
    FirebaseJson r; r.set("error/stringValue", "Kalibrierung bereits aktiv");
    firebase_sync::updateCommandStatus("failed", &r);
    return false;
  }
  if (targetPh != 4.0f && targetPh != 7.0f) {
    FirebaseJson r; r.set("error/stringValue", "phValue muss 4.0 oder 7.0 sein");
    firebase_sync::updateCommandStatus("failed", &r);
    return false;
  }
  phCal.active = true;
  phCal.cmdId = cmdId;
  phCal.targetPh = targetPh;
  phCal.startMs = millis();
  phCal.voltageSum = 0;
  phCal.sampleCount = 0;
  firebase_sync::updateCommandStatus("running");
  Serial.printf("[PhCal] starte pH-%g Kalibrierung (10s sampling)\n", targetPh);
  return true;
}

void tickPhCalibration() {
  if (!phCal.active) return;
  // 10 Sekunden lang Voltage sammeln
  unsigned long elapsed = millis() - phCal.startMs;
  // pH-Sensor läuft eh in ph_sensor::tick() weiter — wir holen den Mittelwert
  float v = ph_sensor::getVoltage();
  if (!isnan(v)) {
    phCal.voltageSum += v;
    phCal.sampleCount++;
  }
  if (elapsed < 10000) return;

  // Kalibrierung abgeschlossen
  if (phCal.sampleCount == 0) {
    FirebaseJson r; r.set("error/stringValue", "Keine Voltage-Samples");
    firebase_sync::updateCommandStatus("failed", &r);
    phCal.active = false;
    return;
  }
  float avg = phCal.voltageSum / phCal.sampleCount;
  Serial.printf("[PhCal] pH-%g: Voltage = %.4f V (%d Samples)\n",
                phCal.targetPh, avg, phCal.sampleCount);

  // In ph_sensor speichern und in Firestore (gepuffert)
  if (phCal.targetPh == 4.0f) ph_sensor::voltagePH4 = avg;
  else if (phCal.targetPh == 7.0f) ph_sensor::voltagePH7 = avg;
  ph_sensor::calibrated = !isnan(ph_sensor::voltagePH4) && !isnan(ph_sensor::voltagePH7);

  // NVS speichern (überlebt Reboot)
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  if (!isnan(ph_sensor::voltagePH4)) p.putFloat("phV4", ph_sensor::voltagePH4);
  if (!isnan(ph_sensor::voltagePH7)) p.putFloat("phV7", ph_sensor::voltagePH7);
  p.end();

  // In Firestore (gepuffert)
  firebase_sync::writePhCalibration(ph_sensor::voltagePH4, ph_sensor::voltagePH7,
                                     ph_sensor::calibrated);

  FirebaseJson result;
  result.set("voltage/doubleValue", avg);
  result.set("samples/integerValue", phCal.sampleCount);
  result.set("phValue/doubleValue", phCal.targetPh);
  result.set("durationMs/integerValue", (int)elapsed);
  result.set("isFullyCalibrated/booleanValue", ph_sensor::calibrated);
  firebase_sync::updateCommandStatus("done", &result);
  phCal.active = false;
}

// ---------- pH-Probe schreiben — :05 jeder geraden Stunde ----------
// 5 Minuten vor Dosis-Trigger, sodass aktueller pH-Wert in dosings/items
// landet UND beim Trigger um :10 für die Tag/Nacht-Entscheidung verfügbar ist.
void checkPhSampleSchedule() {
  if (!ph_sensor::isCalibrated()) return;  // unsicher ohne Kalibrierung
  time_t now; time(&now);
  if (now < 1700000000) return;
  struct tm t; localtime_r(&now, &t);

  // Trigger: Minute 5 jeder geraden Stunde
  if (t.tm_hour % 2 != 0) return;
  if (t.tm_min < 5 || t.tm_min > 8) return;

  // Schon in dieser Stunde geschrieben?
  if (lastPhSampleAt > 0) {
    struct tm last; localtime_r(&lastPhSampleAt, &last);
    if (last.tm_year == t.tm_year && last.tm_yday == t.tm_yday && last.tm_hour == t.tm_hour) return;
  }

  float ph = ph_sensor::getPH();
  if (isnan(ph)) return;
  firebase_sync::addPhMeasurement(ph);
  lastPhSampleAt = now;
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putULong("lastPhAt", (unsigned long)now);
  p.end();
  Serial.printf("[pH] :05-Sample: %.2f → measurements\n", ph);
}

// ---------- Command-Dispatch (Web-Commands) ----------
bool startCommand(const String &cmdId, const String &action, int pump, float ml, long steps, float phValue) {
  if (current.active || phCal.active) {
    Serial.println("[Cmd] bereits aktiv");
    return false;
  }
  if (action == "stop") {
    pumps::emergencyStop();
    firebase_sync::updateCommandStatus("done");
    return true;
  }
  if (action == "calibratePh") {
    return startPhCalibration(cmdId, phValue);
  }
  if (pump < 0 || pump >= pumps::NUM_PUMPS) {
    FirebaseJson r; r.set("error/stringValue", "ungültige Pumpe");
    firebase_sync::updateCommandStatus("failed", &r);
    return false;
  }

  bool ok = false;
  if (action == "calibrate") {
    if (steps <= 0) {
      FirebaseJson r; r.set("error/stringValue", "steps fehlt");
      firebase_sync::updateCommandStatus("failed", &r);
      return false;
    }
    ok = pumps::runSteps(pump, steps);
  } else if (action == "dose") {
    if (isnan(ml) || ml <= 0) {
      FirebaseJson r; r.set("error/stringValue", "ml fehlt");
      firebase_sync::updateCommandStatus("failed", &r);
      return false;
    }
    ok = pumps::runMl(pump, ml);
  } else {
    FirebaseJson r; r.set("error/stringValue", "unbekannte action");
    firebase_sync::updateCommandStatus("failed", &r);
    return false;
  }
  if (!ok) {
    FirebaseJson r; r.set("error/stringValue", "Pumpe nicht startbar");
    firebase_sync::updateCommandStatus("failed", &r);
    return false;
  }

  current = {};
  current.cmdId = cmdId;
  current.action = action;
  current.pump = pump;
  current.ml = ml;
  current.steps = steps;
  current.startMs = millis();
  current.active = true;
  current.fromPlan = false;
  firebase_sync::updateCommandStatus("running");
  Serial.printf("[Cmd] %s pump=%d %s (id=%s)\n", action.c_str(), pump,
                action == "dose" ? (String("ml=") + ml).c_str()
                                 : (String("steps=") + steps).c_str(),
                cmdId.c_str());
  return true;
}

void finishCommand(bool success, const char* errorMsg = nullptr) {
  if (!current.active) return;
  unsigned long duration = millis() - current.startMs;

  // Statistik: jede Dose (auch manuell, auch failed) zählen
  if (current.action == "dose") {
    time_t now; time(&now);
    if (now > 1700000000) recordDose(now, success);
  }

  if (current.fromPlan) {
    // Plan-Dose: dosings-Subcollection + lastDosageTimeCache aktualisieren
    if (success) {
      time_t now; time(&now);
      if (now > 1700000000) {
        lastDosageTimeCache[current.dosageType] = now;
        saveLastDosageTime(current.dosageType);
      }
    }
    firebase_sync::addDosing(current.pump, current.ml,
                              DT_LABELS[current.dosageType], true, 1.0f, success);
    if (success) firebase_sync::decrementContainerLevel(current.pump, current.ml);
    Serial.printf("[Plan] DOSE %s %s (%lums)\n",
                  DT_LABELS[current.dosageType],
                  success ? "done" : "FAILED", duration);
  } else {
    FirebaseJson result;
    result.set("durationMs/integerValue", (int)duration);
    if (current.action == "calibrate") result.set("actualSteps/integerValue", (int)current.steps);
    else if (current.action == "dose") result.set("actualMl/doubleValue", current.ml);
    if (!success && errorMsg) result.set("error/stringValue", errorMsg);
    firebase_sync::updateCommandStatus(success ? "done" : "failed", &result);
    if (current.action == "dose" && success) {
      firebase_sync::addDosing(current.pump, current.ml, "manual", false, 1.0f, true);
      firebase_sync::decrementContainerLevel(current.pump, current.ml);
    }
    Serial.printf("[Cmd] %s (%lums)\n", success ? "done" : "FAILED", duration);
  }
  current.active = false;

  // Sequenz weiterführen
  startNextQueuedDose();
}

// ---------- Command-Polling (adaptive) ----------
void pollCommands() {
  unsigned long now = millis();
  unsigned long interval = (current.active || !queueEmpty() || phCal.active) ? COMMAND_POLL_FAST_MS : COMMAND_POLL_NORMAL_MS;
  if (lastCommandPollMs != 0 && now - lastCommandPollMs < interval) return;
  lastCommandPollMs = now;
  if (current.active || phCal.active) return;

  FirebaseJson doc;
  if (!firebase_sync::fetchActiveCommand(doc)) return;

  FirebaseJsonData fId, fStatus, fAction, fPump, fMl, fSteps, fPhValue;
  doc.get(fId,      "fields/id/stringValue");
  doc.get(fStatus,  "fields/status/stringValue");
  doc.get(fAction,  "fields/action/stringValue");
  doc.get(fPump,    "fields/pump/integerValue");
  doc.get(fMl,      "fields/ml/doubleValue");
  if (!fMl.success) doc.get(fMl, "fields/ml/integerValue");  // Firestore speichert ganze Zahlen als integer
  doc.get(fSteps,   "fields/steps/integerValue");
  if (!fSteps.success) doc.get(fSteps, "fields/steps/doubleValue");
  doc.get(fPhValue, "fields/phValue/doubleValue");
  if (!fPhValue.success) doc.get(fPhValue, "fields/phValue/integerValue");

  if (!fStatus.success || !fAction.success || !fId.success) return;
  if (fStatus.stringValue != "pending") return;
  // Schon abgearbeitet?
  if (fId.stringValue == lastProcessedCmdId) return;

  String cmdId = fId.stringValue;
  lastProcessedCmdId = cmdId;
  Serial.printf("[Cmd] Pending entdeckt: action=%s id=%s\n", fAction.stringValue.c_str(), cmdId.c_str());

  startCommand(cmdId, fAction.stringValue,
               fPump.success ? (int)fPump.intValue : -1,
               fMl.success ? fMl.floatValue : NAN,
               fSteps.success ? (long)fSteps.intValue : 0,
               fPhValue.success ? fPhValue.floatValue : NAN);
}

// ---------- Helpers für Plan-Iteration ----------
struct PlanEntry {
  time_t date;
  float dosageML;       // KH
  float caDosageML;     // Ca
  float mgDosageML;     // Mg
  bool isMaintenanceDose;
  bool valid;
};

PlanEntry parseEntry(FirebaseJson &entry, bool isKH) {
  PlanEntry pe = { 0, 0, 0, 0, false, false };
  FirebaseJsonData v;
  if (entry.get(v, "mapValue/fields/date/integerValue") && v.success) pe.date = (time_t)v.intValue;
  if (isKH) {
    if (entry.get(v, "mapValue/fields/dosageML/doubleValue") && v.success) pe.dosageML = v.floatValue;
  } else {
    if (entry.get(v, "mapValue/fields/caDosageML/doubleValue") && v.success) pe.caDosageML = v.floatValue;
    if (entry.get(v, "mapValue/fields/mgDosageML/doubleValue") && v.success) pe.mgDosageML = v.floatValue;
  }
  if (entry.get(v, "mapValue/fields/isMaintenanceDose/booleanValue") && v.success)
    pe.isMaintenanceDose = (v.stringValue == "true");
  pe.valid = true;
  return pe;
}

// Liefert true wenn lastTs in der gleichen Stunde wie now liegt (lokale Zeit)
bool sameLocalHour(time_t lastTs, time_t now) {
  if (lastTs == 0) return false;
  struct tm a, b;
  localtime_r(&lastTs, &a);
  localtime_r(&now, &b);
  return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday && a.tm_hour == b.tm_hour;
}

// Hole Plan-Array (khEntries oder caEntries) als FirebaseJsonArray
bool getPlanArray(FirebaseJson &doc, const char* fieldKey, FirebaseJsonArray &out) {
  String basePath = String("fields/") + fieldKey + "/arrayValue/values";
  FirebaseJsonData arrData;
  if (!doc.get(arrData, basePath.c_str())) return false;
  arrData.get(out);
  return true;
}

// ---------- Auto-Dosing-Sequenz prüfen ----------
// Wird alle 30s aufgerufen, fired aber nur in Minute 10-15 gerader Stunden.
void checkAutoDosingSequence() {
  unsigned long ms = millis();
  if (lastSequenceCheckMs != 0 && ms - lastSequenceCheckMs < SEQUENCE_CHECK_INTERVAL_MS) return;
  lastSequenceCheckMs = ms;

  if (current.active || !queueEmpty()) return;
  if (!settings_cache::autoDosing) return;  // Master-Schalter aus → keine Auto-Doses
  if (cachedPlanJson.length() == 0) return;
  if (isPlanCacheStale()) {
    Serial.println("[Plan] Cache zu alt (>25h) — keine Auto-Doses");
    return;
  }

  time_t now; time(&now);
  if (now < 1700000000) return;
  struct tm t;
  localtime_r(&now, &t);

  // Trigger-Fenster: Minute 10-15 in gerader Stunde
  if (t.tm_hour % 2 != 0 || t.tm_min < 10 || t.tm_min > 15) return;

  FirebaseJson doc;
  doc.setJsonData(cachedPlanJson);

  // Stunden-Fenster für AUSGLEICH-Detection
  time_t hourStart = now - t.tm_min * 60 - t.tm_sec;
  time_t hourEnd = hourStart + 3600 - 1;

  // ---------- KH-Plan analysieren ----------
  FirebaseJsonArray khArr;
  bool hasKHPlan = getPlanArray(doc, "khEntries", khArr);
  float khDosageML = 0;
  bool khAdjustment = false;
  if (hasKHPlan) {
    // 1) Spezifische AUSGLEICH-Dose für diese Stunde?
    for (size_t i = 0; i < khArr.size(); i++) {
      FirebaseJsonData ed; khArr.get(ed, i);
      FirebaseJson entry; entry.setJsonData(ed.stringValue);
      PlanEntry pe = parseEntry(entry, true);
      if (pe.isMaintenanceDose || pe.date == 0) continue;
      if (pe.date >= hourStart && pe.date <= hourEnd) {
        khDosageML = pe.dosageML;
        khAdjustment = true;
        break;
      }
    }
    // 2) ERHALTUNG (nur wenn kein Ausgleich für diese Stunde)
    if (!khAdjustment) {
      for (size_t i = 0; i < khArr.size(); i++) {
        FirebaseJsonData ed; khArr.get(ed, i);
        FirebaseJson entry; entry.setJsonData(ed.stringValue);
        PlanEntry pe = parseEntry(entry, true);
        if (pe.isMaintenanceDose || pe.date == 0) {
          khDosageML = pe.dosageML;
          break;
        }
      }
    }
  }

  // ---------- Ca-Plan analysieren ----------
  FirebaseJsonArray caArr;
  bool hasCaPlan = getPlanArray(doc, "caEntries", caArr);
  float caDosageML = 0, mgDosageML = 0;
  bool caAdjustment = false;
  if (hasCaPlan) {
    for (size_t i = 0; i < caArr.size(); i++) {
      FirebaseJsonData ed; caArr.get(ed, i);
      FirebaseJson entry; entry.setJsonData(ed.stringValue);
      PlanEntry pe = parseEntry(entry, false);
      if (pe.isMaintenanceDose || pe.date == 0) continue;
      if (pe.date >= hourStart && pe.date <= hourEnd) {
        caDosageML = pe.caDosageML;
        mgDosageML = pe.mgDosageML;
        caAdjustment = true;
        break;
      }
    }
    if (!caAdjustment) {
      for (size_t i = 0; i < caArr.size(); i++) {
        FirebaseJsonData ed; caArr.get(ed, i);
        FirebaseJson entry; entry.setJsonData(ed.stringValue);
        PlanEntry pe = parseEntry(entry, false);
        if (pe.isMaintenanceDose || pe.date == 0) {
          caDosageML = pe.caDosageML;
          mgDosageML = pe.mgDosageML;
          break;
        }
      }
    }
  }

  // ---------- KH-Tag/Nacht-Entscheidung ----------
  bool isNight = settings_cache::isKHNightNow(ph_sensor::getPH());
  DosageType khType = isNight ? DT_KH_NIGHT : DT_KH_DAY;
  int khPump = isNight ? 3 : 2;
  // KH-Nacht-Konzentrat ist 2× konzentriert → halbe Menge
  if (isNight) khDosageML *= 0.5f;

  // ---------- Per-Stunde-Dedup: schon dosiert? ----------
  bool needKH = khDosageML > 0.001f && !sameLocalHour(lastDosageTimeCache[khType], now);
  bool needCa = caDosageML > 0.001f && !sameLocalHour(lastDosageTimeCache[DT_CA], now);
  bool needMg = mgDosageML > 0.001f && !sameLocalHour(lastDosageTimeCache[DT_MG], now);

  if (!needKH && !needCa && !needMg) return;

  Serial.printf("[Plan] Sequenz Stunde %d: KH=%.2fml(%s%s) Ca=%.2fml Mg=%.2fml\n",
                t.tm_hour,
                khDosageML, khAdjustment ? "Ausgleich" : "Erhaltung",
                isNight ? "/Nacht" : "/Tag",
                caDosageML, mgDosageML);

  // ---------- Queue füllen ----------
  if (needKH) enqueueDose(khPump, khDosageML, khType);
  if (needCa) enqueueDose(0, caDosageML, DT_CA);
  if (needMg) enqueueDose(1, mgDosageML, DT_MG);

  // Erste Dose starten
  startNextQueuedDose();
}

// ---------- Pump-Status checken ----------
void checkPumpFinished() {
  int finished = pumps::checkAndDisable();
  if (finished >= 0 && current.active) finishCommand(true);
}

// ---------- Init & Tick ----------
void begin() {
  loadFromNVS();
  settings_cache::begin();
  // pH-Kalibrierung aus NVS laden
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  float v4 = p.getFloat("phV4", NAN);
  float v7 = p.getFloat("phV7", NAN);
  lastPhSampleAt = (time_t)p.getULong("lastPhAt", 0);
  p.end();
  ph_sensor::setCalibration(v4, v7);
  if (ph_sensor::isCalibrated()) {
    Serial.printf("[pH] Kalibrierung aus NVS: V4=%.4f V7=%.4f\n", v4, v7);
  }
}

void tick() {
  checkPumpFinished();
  tickPhCalibration();             // pH-Kalibrierungs-Sampling
  pollCommands();
  syncPumpConfigs();
  settings_cache::sync();
  syncPlan();
  checkPhSampleSchedule();         // pH-Probe um :05 schreiben
  checkAutoDosingSequence();       // Dosier-Sequenz um :10
  firebase_sync::flushBuffer();    // Backlog-Uploads versuchen
}

} // namespace plan_executor
