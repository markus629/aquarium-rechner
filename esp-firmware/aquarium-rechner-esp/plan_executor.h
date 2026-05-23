// =============================================================
// Plan-Executor + Command-Dispatcher (Session B + C komplett)
// =============================================================
// Verarbeitet:
//  - Commands aus Firestore (calibrate, dose, stop)
//  - Plan-Einträge (Auto-Doses inkl. KH-Tag/Nacht, Erhaltung, Mg)
// Cached Plan und Settings im NVS — überlebt Reboot bis PLAN_CACHE_TTL_MS
// =============================================================
#pragma once

#include <Preferences.h>
#include "config.h"
#include "firebase_sync.h"
#include "pumps.h"
#include "ph_sensor.h"
#include "settings_cache.h"

namespace plan_executor {

unsigned long lastPlanCheckMs = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastPumpConfigSyncMs = 0;
unsigned long lastPlanFetchMs = 0;
const unsigned long PLAN_FETCH_INTERVAL_MS = 30 * 1000UL;  // 30s = Quasi-Push

// Aktuelles ausführendes Command (Web-Command oder Plan-Dose)
struct ActiveCmd {
  String docPath;        // bei Web-Command: docPath für Bestätigung; bei Plan-Dose: leer
  String action;
  int pump = -1;
  float ml = NAN;
  long steps = 0;
  unsigned long startMs = 0;
  bool active = false;
  bool fromPlan = false;  // true wenn aus Plan, dann keine Command-Bestätigung sondern Dosing-Log
  String dosageType;      // "kh-day"|"kh-night"|"ca"|"mg"|"kh-maintenance"|"ca-maintenance"
};
ActiveCmd current;

// ---------- Dose-Queue (für aufeinanderfolgende Doses: Ca + Mg) ----------
struct QueuedDose {
  int pump;
  float ml;
  String dosageType;
};
static const int MAX_QUEUE = 4;
QueuedDose doseQueue[MAX_QUEUE];
int queueHead = 0, queueTail = 0;

bool queueEmpty() { return queueHead == queueTail; }
bool queueFull()  { return ((queueTail + 1) % MAX_QUEUE) == queueHead; }
void enqueueDose(int pump, float ml, const String &type) {
  if (queueFull()) { Serial.println("[Queue] FULL — drop"); return; }
  doseQueue[queueTail] = { pump, ml, type };
  queueTail = (queueTail + 1) % MAX_QUEUE;
}
bool dequeueDose(QueuedDose &out) {
  if (queueEmpty()) return false;
  out = doseQueue[queueHead];
  queueHead = (queueHead + 1) % MAX_QUEUE;
  return true;
}

// ---------- Doppelausführungs-Schutz ----------
// Ring-Buffer von Timestamps die schon ausgeführt wurden (KH + Ca getrennt)
static const int MAX_EXECUTED_RECENT = 32;
time_t executedKH[MAX_EXECUTED_RECENT] = {0};
time_t executedCa[MAX_EXECUTED_RECENT] = {0};
int executedHeadKH = 0, executedHeadCa = 0;

bool wasExecuted(time_t ts, time_t *arr) {
  for (int i = 0; i < MAX_EXECUTED_RECENT; i++) if (arr[i] == ts) return true;
  return false;
}
void markExecuted(time_t ts, time_t *arr, int &head) {
  arr[head] = ts;
  head = (head + 1) % MAX_EXECUTED_RECENT;
}

// ---------- Plan-Cache (NVS) ----------
String cachedPlanJson = "";
unsigned long cachedPlanTimeMs = 0;
// Maintenance-Tracking: letzte Erhaltungs-Dose pro Typ
time_t lastMaintenanceKH = 0;
time_t lastMaintenanceCa = 0;

void loadFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  cachedPlanJson = p.getString("planJson", "");
  cachedPlanTimeMs = (unsigned long)p.getULong("planTime", 0);
  lastMaintenanceKH = (time_t)p.getULong("maintKH", 0);
  lastMaintenanceCa = (time_t)p.getULong("maintCa", 0);
  p.end();
  if (cachedPlanJson.length() > 0) {
    Serial.printf("[Plan] Cache aus NVS: %d Zeichen\n", cachedPlanJson.length());
  }
}

void savePlanCacheToNVS(const String &json) {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putString("planJson", json);
  p.putULong("planTime", (unsigned long)(millis()));
  p.end();
}

void saveMaintenanceTimestamps() {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putULong("maintKH", (unsigned long)lastMaintenanceKH);
  p.putULong("maintCa", (unsigned long)lastMaintenanceCa);
  p.end();
}

// ---------- Pump-Configs alle 5 Min syncen ----------
void syncPumpConfigs() {
  unsigned long now = millis();
  if (lastPumpConfigSyncMs != 0 && now - lastPumpConfigSyncMs < 5 * 60 * 1000UL) return;
  lastPumpConfigSyncMs = now;
  for (int i = 0; i < pumps::NUM_PUMPS; i++) {
    float v;
    if (firebase_sync::fetchPumpMlPerStep(i, v)) {
      if (v != pumps::mlPerStep[i]) {
        pumps::setMlPerStep(i, v);
        Serial.printf("[PumpConfig] Pumpe %d: mlPerStep=%.5f\n", i, v);
      }
    }
  }
}

// ---------- Plan alle 30s syncen (quasi-push) ----------
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

// ---------- Dose aus Plan starten ----------
bool startPlanDose(int pump, float ml, const String &type) {
  if (current.active) {
    // verzögern via Queue
    enqueueDose(pump, ml, type);
    return false;
  }
  if (!pumps::runMl(pump, ml)) {
    Serial.printf("[Plan] Pumpe %d konnte nicht gestartet (busy oder nicht kalibriert)\n", pump);
    return false;
  }
  current.docPath = "";
  current.action = "dose";
  current.pump = pump;
  current.ml = ml;
  current.steps = 0;
  current.startMs = millis();
  current.active = true;
  current.fromPlan = true;
  current.dosageType = type;
  Serial.printf("[Plan] DOSE %s pump=%d ml=%.2f\n", type.c_str(), pump, ml);
  return true;
}

// ---------- Command-Dispatch ----------
bool startCommand(const String &docPath, const String &action, int pump, float ml, long steps) {
  if (current.active) {
    Serial.println("[Cmd] bereits aktiv");
    return false;
  }
  if (action == "stop") {
    pumps::emergencyStop();
    firebase_sync::updateCommand(docPath, "done");
    return true;
  }
  if (pump < 0 || pump >= pumps::NUM_PUMPS) {
    FirebaseJson r; r.set("error/stringValue", "ungültige Pumpe");
    firebase_sync::updateCommand(docPath, "failed", &r);
    return false;
  }

  bool ok = false;
  if (action == "calibrate") {
    if (steps <= 0) {
      FirebaseJson r; r.set("error/stringValue", "steps fehlt");
      firebase_sync::updateCommand(docPath, "failed", &r);
      return false;
    }
    ok = pumps::runSteps(pump, steps);
  } else if (action == "dose") {
    if (isnan(ml) || ml <= 0) {
      FirebaseJson r; r.set("error/stringValue", "ml fehlt");
      firebase_sync::updateCommand(docPath, "failed", &r);
      return false;
    }
    ok = pumps::runMl(pump, ml);
  } else {
    FirebaseJson r; r.set("error/stringValue", "unbekannte action");
    firebase_sync::updateCommand(docPath, "failed", &r);
    return false;
  }

  if (!ok) {
    FirebaseJson r; r.set("error/stringValue", "Pumpe nicht startbar (busy oder unkalibriert)");
    firebase_sync::updateCommand(docPath, "failed", &r);
    return false;
  }

  current.docPath = docPath;
  current.action = action;
  current.pump = pump;
  current.ml = ml;
  current.steps = steps;
  current.startMs = millis();
  current.active = true;
  current.fromPlan = false;
  current.dosageType = "manual";
  firebase_sync::updateCommand(docPath, "running");
  Serial.printf("[Cmd] gestartet: %s pump=%d %s\n", action.c_str(), pump,
                action == "dose" ? (String("ml=") + ml).c_str() : (String("steps=") + steps).c_str());
  return true;
}

void finishCommand(bool success, const char* errorMsg = nullptr) {
  if (!current.active) return;
  unsigned long duration = millis() - current.startMs;

  if (current.fromPlan) {
    // Aus Plan: direkt in dosings-Subcollection loggen, kein Command-Doc
    firebase_sync::addDosing(current.pump, current.ml, current.dosageType.c_str(),
                              true, 1.0f, success);
    Serial.printf("[Plan] DOSE %s (%lu ms)\n", success ? "done" : "FAILED", duration);
  } else {
    // Web-Command: Command-Doc aktualisieren + dosings-Eintrag bei Dose
    FirebaseJson result;
    result.set("durationMs/integerValue", (int)duration);
    if (current.action == "calibrate") result.set("actualSteps/integerValue", (int)current.steps);
    else if (current.action == "dose") result.set("actualMl/doubleValue", current.ml);
    if (!success && errorMsg) result.set("error/stringValue", errorMsg);
    firebase_sync::updateCommand(current.docPath, success ? "done" : "failed", &result);
    if (current.action == "dose" && success) {
      firebase_sync::addDosing(current.pump, current.ml, "manual", false, 1.0f, true);
    }
    Serial.printf("[Cmd] %s (%lu ms)\n", success ? "done" : "FAILED", duration);
  }
  current.active = false;

  // Queue: nächste Dose starten
  QueuedDose next;
  if (dequeueDose(next)) {
    Serial.printf("[Queue] nächste Dose: pump=%d ml=%.2f\n", next.pump, next.ml);
    startPlanDose(next.pump, next.ml, next.dosageType);
  }
}

// ---------- Command-Polling (adaptive) ----------
void pollCommands() {
  unsigned long now = millis();
  unsigned long interval = current.active ? COMMAND_POLL_FAST_MS : COMMAND_POLL_NORMAL_MS;
  if (lastCommandPollMs != 0 && now - lastCommandPollMs < interval) return;
  lastCommandPollMs = now;
  if (current.active) return;

  FirebaseJsonArray pending;
  if (!firebase_sync::fetchPendingCommands(pending)) return;
  if (pending.size() == 0) return;

  FirebaseJsonData item;
  pending.get(item, 0);
  FirebaseJson cmd;
  cmd.setJsonData(item.stringValue);

  FirebaseJsonData fName, fAction, fPump, fMl, fSteps;
  cmd.get(fName, "document/name");
  cmd.get(fAction, "document/fields/action/stringValue");
  cmd.get(fPump, "document/fields/pump/integerValue");
  cmd.get(fMl, "document/fields/ml/doubleValue");
  cmd.get(fSteps, "document/fields/steps/integerValue");
  if (!fName.success || !fAction.success) return;

  String fullName = fName.stringValue;
  int idx = fullName.indexOf("/documents/");
  String docPath = idx >= 0 ? fullName.substring(idx + 11) : fullName;

  startCommand(docPath, fAction.stringValue,
               fPump.success ? (int)fPump.intValue : -1,
               fMl.success ? fMl.floatValue : NAN,
               fSteps.success ? (long)fSteps.intValue : 0);
}

// ---------- Plan-Check (alle 60 s) ----------
// Geht Plan-Einträge durch und feuert fällige Doses.
//  - Spezifisch datierte Einträge (date != 0): fire wenn jetzt ± 90 s
//  - Erhaltungs-Einträge (date == 0 / isMaintenanceDose): fire wenn 2h seit
//    letzter Erhaltungs-Dose desselben Typs vergangen UND keine spezifische
//    Dose innerhalb der nächsten 90 s ansteht
void checkPlanForDueDose() {
  unsigned long ms = millis();
  if (lastPlanCheckMs != 0 && ms - lastPlanCheckMs < PLAN_CHECK_INTERVAL_MS) return;
  lastPlanCheckMs = ms;

  if (current.active) return;
  if (cachedPlanJson.length() == 0) return;
  if (isPlanCacheStale()) {
    Serial.println("[Plan] Cache zu alt (>25h) — keine Auto-Doses");
    return;
  }
  time_t now; time(&now);
  if (now < 1700000000) return;

  FirebaseJson doc;
  doc.setJsonData(cachedPlanJson);

  // Pro Typ: bestimme ob spezifische Dose oder Erhaltung dran ist
  auto processType = [&](bool isKH) {
    const char* arrKey = isKH ? "khEntries" : "caEntries";
    String basePath = String("fields/") + arrKey + "/arrayValue/values";
    FirebaseJsonData arrData;
    if (!doc.get(arrData, basePath.c_str())) return;
    FirebaseJsonArray entries;
    arrData.get(entries);

    bool hasUpcomingSpecific = false;
    int maintenanceIdx = -1;

    // Erster Durchgang: spezifische Doses checken
    for (size_t i = 0; i < entries.size(); i++) {
      FirebaseJsonData itemData;
      entries.get(itemData, i);
      FirebaseJson entry;
      entry.setJsonData(itemData.stringValue);
      FirebaseJsonData dateF, mlF, isMaintF;
      entry.get(dateF, "mapValue/fields/date/integerValue");
      const char* mlField = isKH ? "mapValue/fields/dosageML/doubleValue"
                                  : "mapValue/fields/caDosageML/doubleValue";
      entry.get(mlF, mlField);
      entry.get(isMaintF, "mapValue/fields/isMaintenanceDose/booleanValue");

      time_t date = dateF.success ? (time_t)dateF.intValue : 0;
      float ml = mlF.success ? mlF.floatValue : 0;
      bool isMaint = isMaintF.success && (isMaintF.stringValue == "true");

      if (isMaint || date == 0) {
        maintenanceIdx = (int)i;
        continue;
      }
      if (ml <= 0.001f) continue;

      time_t *exArr = isKH ? executedKH : executedCa;
      int &exHead = isKH ? executedHeadKH : executedHeadCa;

      // Fällig im Fenster ± 90 s
      if (date >= now - 90 && date <= now + 90) {
        if (wasExecuted(date, exArr)) continue;
        // KH: Tag/Nacht-Entscheidung
        int pump;
        String type;
        if (isKH) {
          bool isNight = settings_cache::isKHNightNow(ph_sensor::getPH());
          pump = isNight ? 3 : 2;
          type = isNight ? "kh-night" : "kh-day";
        } else {
          pump = 0;
          type = "ca";
        }
        Serial.printf("[Plan] Fällige %s-Dose ml=%.2f pump=%d (date=%lu)\n",
                      isKH ? "KH" : "Ca", ml, pump, (unsigned long)date);
        markExecuted(date, exArr, exHead);
        startPlanDose(pump, ml, type);
        // Bei Ca: Mg parallel (in Queue)
        if (!isKH) {
          FirebaseJsonData mgF;
          entry.get(mgF, "mapValue/fields/mgDosageML/doubleValue");
          float mgML = mgF.success ? mgF.floatValue : 0;
          if (mgML > 0.001f) {
            enqueueDose(1, mgML, "mg");
          }
        }
        return;  // nur eine Plan-Dose pro Check
      }
      // Liegt diese spezifische Dose in den nächsten 2h?
      if (date > now && date <= now + 2 * 3600) hasUpcomingSpecific = true;
    }

    // Wenn keine spezifische Dose in den nächsten 2h ansteht: Erhaltungs-Dose prüfen
    if (maintenanceIdx >= 0 && !hasUpcomingSpecific) {
      time_t &lastMaint = isKH ? lastMaintenanceKH : lastMaintenanceCa;
      if (now - lastMaint >= 2 * 3600 - 60) {  // 2h Toleranz -1 Min
        FirebaseJsonData itemData;
        entries.get(itemData, maintenanceIdx);
        FirebaseJson entry;
        entry.setJsonData(itemData.stringValue);
        FirebaseJsonData mlF;
        const char* mlField = isKH ? "mapValue/fields/dosageML/doubleValue"
                                    : "mapValue/fields/caDosageML/doubleValue";
        entry.get(mlF, mlField);
        float ml = mlF.success ? mlF.floatValue : 0;
        if (ml <= 0.001f) return;

        int pump;
        String type;
        if (isKH) {
          bool isNight = settings_cache::isKHNightNow(ph_sensor::getPH());
          pump = isNight ? 3 : 2;
          type = isNight ? "kh-maintenance-night" : "kh-maintenance-day";
        } else {
          pump = 0;
          type = "ca-maintenance";
        }
        Serial.printf("[Plan] Erhaltungs-%s-Dose ml=%.2f pump=%d\n",
                      isKH ? "KH" : "Ca", ml, pump);
        lastMaint = now;
        saveMaintenanceTimestamps();
        startPlanDose(pump, ml, type);
        if (!isKH) {
          FirebaseJsonData mgF;
          entry.get(mgF, "mapValue/fields/mgDosageML/doubleValue");
          float mgML = mgF.success ? mgF.floatValue : 0;
          if (mgML > 0.001f) enqueueDose(1, mgML, "mg-maintenance");
        }
      }
    }
  };

  processType(true);   // KH
  processType(false);  // Ca + Mg
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
}

void tick() {
  checkPumpFinished();
  pollCommands();
  syncPumpConfigs();
  settings_cache::sync();
  syncPlan();
  checkPlanForDueDose();
}

} // namespace plan_executor
