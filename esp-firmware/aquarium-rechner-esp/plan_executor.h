// =============================================================
// Plan-Executor + Command-Dispatcher
// =============================================================
// Verarbeitet Commands aus Firestore (action: calibrate, dose, stop)
// und Plan-Einträge (geplante Doses).
// =============================================================
#pragma once

#include "config.h"
#include "firebase_sync.h"
#include "pumps.h"
#include "ph_sensor.h"

namespace plan_executor {

unsigned long lastPlanCheckMs = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastPumpConfigSyncMs = 0;

// Aktuelles ausführendes Command (nur eines gleichzeitig)
struct ActiveCmd {
  String docPath;        // "users/{uid}/aquarium-commands/{id}"
  String action;
  int pump = -1;
  float ml = NAN;
  long steps = 0;
  unsigned long startMs = 0;
  bool active = false;
};
ActiveCmd current;

// ---------- Pump-Configs alle 5 Min syncen ----------
// Holt mlPerStep aus Firestore, persistiert in NVS (überlebt Reboot).
void syncPumpConfigs() {
  unsigned long now = millis();
  if (lastPumpConfigSyncMs != 0 && now - lastPumpConfigSyncMs < 5 * 60 * 1000UL) return;
  lastPumpConfigSyncMs = now;
  for (int i = 0; i < pumps::NUM_PUMPS; i++) {
    float v;
    if (firebase_sync::fetchPumpMlPerStep(i, v)) {
      if (v != pumps::mlPerStep[i]) {
        pumps::setMlPerStep(i, v);  // schreibt auch in NVS
        Serial.printf("[PumpConfig] Pumpe %d aktualisiert: mlPerStep=%.5f\n", i, v);
      }
    }
  }
}

// ---------- Command starten ----------
bool startCommand(const String &docPath, const String &action, int pump, float ml, long steps) {
  if (current.active) {
    Serial.println("[Cmd] Bereits ein Command aktiv, neuer wird ignoriert");
    return false;
  }

  // stop: keine Pumpenprüfung nötig
  if (action == "stop") {
    pumps::emergencyStop();
    firebase_sync::updateCommand(docPath, "done");
    return true;
  }

  if (pump < 0 || pump >= pumps::NUM_PUMPS) {
    Serial.printf("[Cmd] ungültige Pumpe: %d\n", pump);
    FirebaseJson result;
    result.set("error/stringValue", "ungültige Pumpe");
    firebase_sync::updateCommand(docPath, "failed", &result);
    return false;
  }

  bool ok = false;
  if (action == "calibrate") {
    if (steps <= 0) {
      Serial.println("[Cmd] calibrate: steps fehlt/ungültig");
      FirebaseJson result;
      result.set("error/stringValue", "steps fehlt");
      firebase_sync::updateCommand(docPath, "failed", &result);
      return false;
    }
    ok = pumps::runSteps(pump, steps);
  } else if (action == "dose") {
    if (isnan(ml) || ml <= 0) {
      Serial.println("[Cmd] dose: ml fehlt/ungültig");
      FirebaseJson result;
      result.set("error/stringValue", "ml fehlt");
      firebase_sync::updateCommand(docPath, "failed", &result);
      return false;
    }
    ok = pumps::runMl(pump, ml);
  } else {
    Serial.printf("[Cmd] unbekannte action: %s\n", action.c_str());
    FirebaseJson result;
    result.set("error/stringValue", "unbekannte action");
    firebase_sync::updateCommand(docPath, "failed", &result);
    return false;
  }

  if (!ok) {
    FirebaseJson result;
    result.set("error/stringValue", "Pumpe konnte nicht gestartet werden (busy oder nicht kalibriert?)");
    firebase_sync::updateCommand(docPath, "failed", &result);
    return false;
  }

  current.docPath = docPath;
  current.action = action;
  current.pump = pump;
  current.ml = ml;
  current.steps = steps;
  current.startMs = millis();
  current.active = true;
  firebase_sync::updateCommand(docPath, "running");
  Serial.printf("[Cmd] gestartet: %s pump=%d %s\n", action.c_str(), pump,
                action == "dose" ? (String("ml=") + ml).c_str()
                                 : (String("steps=") + steps).c_str());
  return true;
}

// ---------- Fertigen Command bestätigen ----------
void finishCommand(bool success, const char* errorMsg = nullptr) {
  if (!current.active) return;
  unsigned long duration = millis() - current.startMs;
  FirebaseJson result;
  result.set("durationMs/integerValue", (int)duration);
  if (current.action == "calibrate") {
    result.set("actualSteps/integerValue", (int)current.steps);
  } else if (current.action == "dose") {
    result.set("actualMl/doubleValue", current.ml);
  }
  if (!success && errorMsg) {
    result.set("error/stringValue", errorMsg);
  }
  firebase_sync::updateCommand(current.docPath, success ? "done" : "failed", &result);

  // Bei dose: zusätzlich in dosings-Subcollection loggen
  if (current.action == "dose" && success) {
    firebase_sync::addDosing(current.pump, current.ml, "auto", true, 1.0f, true);
  }
  Serial.printf("[Cmd] %s nach %lu ms\n", success ? "DONE" : "FAILED", duration);
  current.active = false;
}

// ---------- Command-Polling (Adaptive) ----------
void pollCommands() {
  unsigned long now = millis();
  unsigned long interval = current.active ? COMMAND_POLL_FAST_MS : COMMAND_POLL_NORMAL_MS;
  if (lastCommandPollMs != 0 && now - lastCommandPollMs < interval) return;
  lastCommandPollMs = now;
  if (current.active) return; // nicht polling während wir was machen

  FirebaseJsonArray pending;
  if (!firebase_sync::fetchPendingCommands(pending)) return;
  if (pending.size() == 0) return;

  // Ersten pending Command nehmen (älteste zuerst dank orderBy ASC)
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

  if (!fName.success || !fAction.success) {
    Serial.println("[Cmd] ungültiges Doc-Format, überspringe");
    return;
  }
  // document/name ist "projects/{p}/databases/(default)/documents/users/{uid}/aquarium-commands/{id}"
  // patchDocument erwartet aber NUR den Pfad ab documents/
  String fullName = fName.stringValue;
  int idx = fullName.indexOf("/documents/");
  String docPath = idx >= 0 ? fullName.substring(idx + 11) : fullName;

  String action = fAction.stringValue;
  int pump = fPump.success ? (int)fPump.intValue : -1;
  float ml = fMl.success ? fMl.floatValue : NAN;
  long steps = fSteps.success ? (long)fSteps.intValue : 0;

  startCommand(docPath, action, pump, ml, steps);
}

// ---------- Plan-Cache (NVS) ----------
// Wir cachen den Plan als JSON-String + Zeitstempel + Liste der schon-ausgeführten Doses
// (um Doppelausführung zu verhindern).
unsigned long lastPlanFetchMs = 0;
String cachedPlanJson = "";
unsigned long cachedPlanTimeMs = 0;
static const int MAX_EXECUTED_RECENT = 32;
time_t executedDoses[MAX_EXECUTED_RECENT] = {0};  // Timestamps schon-ausgeführter KH-Doses
time_t executedDosesCa[MAX_EXECUTED_RECENT] = {0};
int executedHead = 0, executedHeadCa = 0;

bool wasExecuted(time_t ts, time_t *arr) {
  for (int i = 0; i < MAX_EXECUTED_RECENT; i++) {
    if (arr[i] == ts) return true;
  }
  return false;
}
void markExecuted(time_t ts, time_t *arr, int &head) {
  arr[head] = ts;
  head = (head + 1) % MAX_EXECUTED_RECENT;
}

void loadPlanCacheFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  cachedPlanJson = p.getString("planJson", "");
  cachedPlanTimeMs = (unsigned long)p.getULong("planTime", 0);
  p.end();
  if (cachedPlanJson.length() > 0) {
    Serial.printf("[Plan] Cache aus NVS geladen (%d Zeichen)\n", cachedPlanJson.length());
  }
}

void savePlanCacheToNVS(const String &json) {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putString("planJson", json);
  p.putULong("planTime", (unsigned long)(millis()));
  p.end();
}

// ---------- Plan aus Firestore holen ----------
// Versucht jeden 5 Min, schreibt bei Erfolg in NVS-Cache.
void syncPlan() {
  if (!firebase_sync::isReady()) return;
  unsigned long now = millis();
  if (lastPlanFetchMs != 0 && now - lastPlanFetchMs < 5 * 60 * 1000UL) return;
  lastPlanFetchMs = now;

  FirebaseJson doc;
  if (!firebase_sync::fetchPlan(doc)) {
    Serial.println("[Plan] Fetch fehlgeschlagen — nutze Cache falls vorhanden");
    return;
  }
  String json;
  doc.toString(json);
  if (json.length() > 0 && json != cachedPlanJson) {
    cachedPlanJson = json;
    cachedPlanTimeMs = now;
    savePlanCacheToNVS(json);
    Serial.printf("[Plan] aktualisiert (%d Zeichen)\n", json.length());
  }
}

// ---------- Cache-Alter prüfen ----------
bool isPlanCacheStale() {
  if (cachedPlanJson.length() == 0) return true;
  unsigned long age = millis() - cachedPlanTimeMs;
  return age > PLAN_CACHE_TTL_MS;
}

// ---------- Plan-Check (alle 60 s) ----------
// Geht die KH- und Ca-Plan-Einträge durch und führt fällige aus.
// "fällig" = entry.date liegt in der Vergangenheit oder innerhalb der nächsten 90 s.
void checkPlanForDueDose() {
  unsigned long ms = millis();
  if (lastPlanCheckMs != 0 && ms - lastPlanCheckMs < PLAN_CHECK_INTERVAL_MS) return;
  lastPlanCheckMs = ms;

  if (current.active) return;  // erst aktuelle Dose abwarten
  if (cachedPlanJson.length() == 0) return;
  if (isPlanCacheStale()) {
    Serial.println("[Plan] Cache zu alt (>25h) — keine Auto-Doses");
    return;
  }

  time_t now;
  time(&now);
  if (now < 1700000000) return;  // Zeit noch nicht synced

  FirebaseJson doc;
  doc.setJsonData(cachedPlanJson);

  // Helper: Plan-Array (kh oder ca) durchgehen
  auto processArray = [&](const char* fieldKey, bool isKH) {
    String basePath = String("fields/") + fieldKey + "/arrayValue/values";
    FirebaseJsonData arr;
    if (!doc.get(arr, basePath.c_str())) return;
    FirebaseJsonArray entries;
    arr.get(entries);
    for (size_t i = 0; i < entries.size(); i++) {
      FirebaseJsonData itemData;
      entries.get(itemData, i);
      FirebaseJson entry;
      entry.setJsonData(itemData.stringValue);
      // Einträge sind unter mapValue/fields/{date,dosageML,caDosageML,mgDosageML,isMaintenanceDose}
      FirebaseJsonData dateF, mlF, isMaintF;
      entry.get(dateF, "mapValue/fields/date/integerValue");
      const char* mlField = isKH ? "mapValue/fields/dosageML/doubleValue" : "mapValue/fields/caDosageML/doubleValue";
      entry.get(mlF, mlField);
      entry.get(isMaintF, "mapValue/fields/isMaintenanceDose/booleanValue");

      time_t date = dateF.success ? (time_t)dateF.intValue : 0;
      float ml = mlF.success ? mlF.floatValue : 0;
      bool isMaint = isMaintF.success ? (isMaintF.stringValue == "true") : false;

      if (ml <= 0.001f) continue;

      // Erhaltungsdosis (date == 0): TODO — alle 2 Stunden ausführen
      if (date == 0 || isMaint) continue;  // Session C: maintenance pacing

      // Liegt fällig in Vergangenheit oder innerhalb 90s?
      if (date < now - 90 || date > now + 90) continue;

      time_t *exArr = isKH ? executedDoses : executedDosesCa;
      int &exHead = isKH ? executedHead : executedHeadCa;
      if (wasExecuted(date, exArr)) continue;

      // KH: bei isNightDosage und pH-Mode → Pumpe 3 statt 2
      int pump = isKH ? 2 : 0;
      if (isKH) {
        // TODO Session C: Tag/Nacht-Entscheidung anhand pH/Uhrzeit
      }

      Serial.printf("[Plan] Fällige Dose: %s ml=%.2f pump=%d\n", isKH?"KH":"Ca", ml, pump);
      if (pumps::runMl(pump, ml)) {
        markExecuted(date, exArr, exHead);
        // Bestätigung in dosings via Heartbeat-Mechanismus (finishCommand wird's nicht abdecken,
        // da kein Command-Doc — wir loggen direkt):
        firebase_sync::addDosing(pump, ml, isKH?"kh-plan":"ca-plan", true, 1.0f, true);
        break;  // nur eine Dose pro Check
      }

      // Bei Ca-Plan zusätzlich Mg-Pumpe
      if (!isKH) {
        FirebaseJsonData mgF;
        entry.get(mgF, "mapValue/fields/mgDosageML/doubleValue");
        if (mgF.success && mgF.floatValue > 0.001f) {
          // direkt nach Ca: wird im nächsten Tick gefahren
          // Vereinfachung für Session B: erstmal nur die Hauptpumpe
        }
      }
    }
  };

  processArray("khEntries", true);
  processArray("caEntries", false);
}

// ---------- Pump-Status checken (im Loop aufrufen) ----------
void checkPumpFinished() {
  int finished = pumps::checkAndDisable();
  if (finished >= 0 && current.active) {
    finishCommand(true);
  }
}

void begin() {
  loadPlanCacheFromNVS();
}

void tick() {
  checkPumpFinished();
  pollCommands();
  syncPumpConfigs();
  syncPlan();
  checkPlanForDueDose();
}

} // namespace plan_executor
