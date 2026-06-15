// =============================================================
// Plan-Executor + Command-Dispatcher
// =============================================================
// Portiert + erweitert die Logik aus 088_sketch_sep3a_01.ino:
//
// - Trigger-Fenster: NUR in Minute 10-15 einer "Dosier-Stunde".
//   Dosier-Stunde = Stunde wo (hour % intervalHours == 0)
//   bei 12×/Tag: 0,2,4,…,22 / bei 6×/Tag: 0,4,8,12,16,20 / etc.
// - Frequenz wählbar via settings_cache::dosingsPerDay
// - Pro Stunde dedupliziert via lastDosageTimeCache[] pro Typ
// - Pro Typ wird geprüft: gibt es eine spezifische Plan-Dose in dieser
//   Stunde (AUSGLEICH-Phase) oder nicht (ERHALTUNG-Phase)
// - Alle anwendbaren Doses (KH + Ca + Mg) werden in EINE Sequenz
//   gepackt → laufen sequentiell ab via Queue
// - KH-Tag/Nacht-Entscheidung zur Laufzeit (pH-basiert oder Uhrzeit)
// - KH-Nacht: ml × 0.5 (Konzentrat ist doppelt konzentriert)
//
// pH-Probe wird in Minute :05-:08 derselben Dosier-Stunden geschrieben
// (5 Min vor der eigentlichen Dosis, sodass Tag/Nacht-Entscheidung
// einen frischen pH-Wert hat).
//
// Cached Plan + Settings + Kalibrierung + Maintenance-Timestamps im NVS.
// =============================================================
#pragma once

#include <Preferences.h>
#include "config.h"
#include "pb_sync.h"
#include "pumps.h"
#include "ph_sensor.h"
#include "settings_cache.h"
#include "ota_update.h"

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

void loadFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  cachedPlanJson = p.getString("planJson", "");
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
  p.end();
}

// ---------- Pump-Configs alle 5 Min syncen ----------
void syncPumpConfigs() {
  unsigned long now = millis();
  if (lastPumpConfigSyncMs != 0 && now - lastPumpConfigSyncMs < 5UL * 60 * 1000) return;
  lastPumpConfigSyncMs = now;
  for (int i = 0; i < pumps::NUM_PUMPS; i++) {
    float v;
    if (pb_sync::fetchPumpStepsPerML(i, v)) {
      if (v != pumps::stepsPerML[i]) {
        pumps::setStepsPerML(i, v);
        Serial.printf("[PumpConfig] Pumpe %d: %.2f Schritte/ml\n", i, v);
      }
    }
  }
}

// ---------- Plan alle 30s syncen (Quasi-Push) ----------
void syncPlan() {
  if (!pb_sync::isReady()) return;
  unsigned long now = millis();
  if (lastPlanFetchMs != 0 && now - lastPlanFetchMs < PLAN_FETCH_INTERVAL_MS) return;
  lastPlanFetchMs = now;

  JsonDocument doc;
  if (!pb_sync::fetchPlan(doc)) return;
  String json;
  serializeJson(doc, json);
  if (json.length() > 0 && json != cachedPlanJson) {
    cachedPlanJson = json;
    savePlanCacheToNVS(json);
    Serial.printf("[Plan] aktualisiert (%d Zeichen)\n", json.length());
  }
}

// Plan ist nur dann "stale" wenn gar keiner gecached ist.
// Kein Zeit-Limit: Anpassungs-Doses haben absolute Timestamps und werden
// nach Ablauf einfach übersprungen — übrig bleibt die Erhaltungsdose
// (date: 0), die unbegrenzt gültig ist. Sobald der User wieder misst oder
// Settings ändert, wird der Plan automatisch ersetzt.
bool isPlanCacheStale() {
  return cachedPlanJson.length() == 0;
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
    JsonDocument r; r["error"] = "Kalibrierung bereits aktiv";
    pb_sync::updateCommandStatus("failed", &r);
    return false;
  }
  if (targetPh != 4.0f && targetPh != 7.0f) {
    JsonDocument r; r["error"] = "phValue muss 4.0 oder 7.0 sein";
    pb_sync::updateCommandStatus("failed", &r);
    return false;
  }
  phCal.active = true;
  phCal.cmdId = cmdId;
  phCal.targetPh = targetPh;
  phCal.startMs = millis();
  phCal.voltageSum = 0;
  phCal.sampleCount = 0;
  pb_sync::updateCommandStatus("running");
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
    JsonDocument r; r["error"] = "Keine Voltage-Samples";
    pb_sync::updateCommandStatus("failed", &r);
    phCal.active = false;
    return;
  }
  float avg = phCal.voltageSum / phCal.sampleCount;
  Serial.printf("[PhCal] pH-%g: Voltage = %.4f V (%d Samples)\n",
                phCal.targetPh, avg, phCal.sampleCount);

  // In ph_sensor speichern und in PocketBase (gepuffert)
  if (phCal.targetPh == 4.0f) ph_sensor::voltagePH4 = avg;
  else if (phCal.targetPh == 7.0f) ph_sensor::voltagePH7 = avg;
  ph_sensor::calibrated = !isnan(ph_sensor::voltagePH4) && !isnan(ph_sensor::voltagePH7);

  // NVS speichern (überlebt Reboot)
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  if (!isnan(ph_sensor::voltagePH4)) p.putFloat("phV4", ph_sensor::voltagePH4);
  if (!isnan(ph_sensor::voltagePH7)) p.putFloat("phV7", ph_sensor::voltagePH7);
  p.end();

  // In PocketBase (gepuffert)
  pb_sync::writePhCalibration(ph_sensor::voltagePH4, ph_sensor::voltagePH7,
                                     ph_sensor::calibrated);

  JsonDocument result;
  result["voltage"] = avg;
  result["samples"] = phCal.sampleCount;
  result["phValue"] = phCal.targetPh;
  result["durationMs"] = (int)elapsed;
  result["isFullyCalibrated"] = ph_sensor::calibrated;
  pb_sync::updateCommandStatus("done", &result);
  phCal.active = false;
}

// ---------- pH-Graph-Probe — fix alle 2 Stunden ----------
// Unabhängig vom Dosier-Plan: schreibt alle 2 h EINEN pH-Wert in die
// Messungen (für gleichmäßige Verlaufs-Graphen im UI). Ausgerichtet auf
// ein festes 2-h-Raster (now/7200) → genau ein Punkt pro Slot.
void checkPhSampleSchedule() {
  if (!ph_sensor::isCalibrated()) return;  // unsicher ohne Kalibrierung
  time_t now; time(&now);
  if (now < 1700000000) return;

  long slot = (long)(now / 7200);            // 2-h-Slot
  static long lastSlot = -1;
  if (lastSlot < 0 && lastPhSampleAt > 0) lastSlot = (long)(lastPhSampleAt / 7200);
  if (slot == lastSlot) return;              // in diesem Slot schon geschrieben

  float ph = ph_sensor::getPH();
  if (isnan(ph)) return;
  pb_sync::addPhMeasurement(ph);
  lastSlot = slot;
  lastPhSampleAt = now;
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putULong("lastPhAt", (unsigned long)now);
  p.end();
  Serial.printf("[pH] 2h-Sample: %.2f → measurements\n", ph);
}

// ---------- Abbruch: laufende Aktion + Queue stoppen ----------
// Wird vom Stop-Command gerufen — funktioniert auch MITTEN in einer
// laufenden Dose/Kalibrierung (pollCommands lässt stop immer durch).
void abortAll() {
  pumps::emergencyStop();
  queueHead = queueTail;  // restliche Sequenz-Queue verwerfen
  if (current.active) {
    // Abgebrochene Dose als failed dokumentieren (Statistik + Verlauf)
    if (current.action == "dose") {
      time_t now; time(&now);
      if (now > 1700000000) recordDose(now, false);
      pb_sync::addDosing(current.pump, current.ml,
                               current.fromPlan ? DT_LABELS[current.dosageType] : "manual",
                               current.fromPlan, 1.0f, false);
    }
    current.active = false;
  }
  phCal.active = false;
  Serial.println("[Cmd] ABBRUCH — Aktion + Queue gestoppt");
}

// ---------- Command-Dispatch (Web-Commands) ----------
bool startCommand(const String &cmdId, const String &action, int pump, float ml, long steps, float phValue) {
  if (current.active || phCal.active) {
    Serial.println("[Cmd] bereits aktiv");
    return false;
  }
  if (action == "stop") {
    abortAll();
    pb_sync::updateCommandStatus("done");
    return true;
  }
  if (action == "otaCheck") {
    pb_sync::updateCommandStatus("running");
    // 1) GitHub-Release holen (~2 s)
    bool found = ota_update::fetchLatestRelease();
    JsonDocument r;
    r["currentVersion"] = FW_VERSION;
    r["availableVersion"] = ota_update::availableVersion;
    if (!found) {
      r["info"] = "Kein GitHub-Release gefunden";
      pb_sync::updateCommandStatus("done", &r);
      return true;
    }
    int cmp = ota_update::compareVersions(FW_VERSION, ota_update::availableVersion);
    if (cmp >= 0) {
      r["info"] = "Bereits aktuell";
      pb_sync::updateCommandStatus("done", &r);
      return true;
    }
    if (ota_update::isProtectedWindow()) {
      r["info"] = "Update verschoben — Dosier-Schutzfenster (:08-:20)";
      pb_sync::updateCommandStatus("done", &r);
      return true;
    }
    // 2) Update wird durchgeführt — Web vorher informieren, sonst Timeout beim Reboot
    r["info"] = "Update läuft — ESP startet gleich neu";
    pb_sync::updateCommandStatus("done", &r);
    delay(500);  // PATCH Zeit zum Committen geben
    // 3) Jetzt der eigentliche Download + Reboot (blockt mehrere Sekunden)
    ota_update::performUpdate(ota_update::availableUrl);
    // Falls performUpdate scheitert (kein Reboot), Status nochmal melden
    JsonDocument r2;
    r2["currentVersion"] = FW_VERSION;
    r2["availableVersion"] = ota_update::availableVersion;
    r2["info"] = "Update fehlgeschlagen — siehe ESP-Serial";
    pb_sync::updateCommandStatus("failed", &r2);
    return true;
  }
  if (action == "calibratePh") {
    return startPhCalibration(cmdId, phValue);
  }
  if (pump < 0 || pump >= pumps::NUM_PUMPS) {
    JsonDocument r; r["error"] = "ungültige Pumpe";
    pb_sync::updateCommandStatus("failed", &r);
    return false;
  }

  bool ok = false;
  if (action == "calibrate") {
    if (steps <= 0) {
      JsonDocument r; r["error"] = "steps fehlt";
      pb_sync::updateCommandStatus("failed", &r);
      return false;
    }
    ok = pumps::runSteps(pump, steps);
  } else if (action == "dose") {
    if (isnan(ml) || ml <= 0) {
      JsonDocument r; r["error"] = "ml fehlt";
      pb_sync::updateCommandStatus("failed", &r);
      return false;
    }
    ok = pumps::runMl(pump, ml);
  } else {
    JsonDocument r; r["error"] = "unbekannte action";
    pb_sync::updateCommandStatus("failed", &r);
    return false;
  }
  if (!ok) {
    JsonDocument r; r["error"] = "Pumpe nicht startbar";
    pb_sync::updateCommandStatus("failed", &r);
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
  pb_sync::updateCommandStatus("running");
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
    // Kanister-Dekrement übernimmt flushBuffer beim Upload der Dose
    // (offline-sicher — geht im Urlaubs-Puffer nicht verloren)
    pb_sync::addDosing(current.pump, current.ml,
                              DT_LABELS[current.dosageType], true, 1.0f, success);
    Serial.printf("[Plan] DOSE %s %s (%lums)\n",
                  DT_LABELS[current.dosageType],
                  success ? "done" : "FAILED", duration);
  } else {
    JsonDocument result;
    result["durationMs"] = (int)duration;
    if (current.action == "calibrate") result["actualSteps"] = (int)current.steps;
    else if (current.action == "dose") result["actualMl"] = current.ml;
    if (!success && errorMsg) result["error"] = errorMsg;
    pb_sync::updateCommandStatus(success ? "done" : "failed", &result);
    if (current.action == "dose" && success) {
      // Kanister-Dekrement übernimmt flushBuffer beim Upload
      pb_sync::addDosing(current.pump, current.ml, "manual", false, 1.0f, true);
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
  // WICHTIG: auch während laufender Aktion weiter pollen — sonst kommt
  // ein Stop-Command nie an. Nicht-Stop-Commands warten unten einfach.

  JsonDocument doc;
  if (!pb_sync::fetchActiveCommand(doc)) return;

  String cmdId  = doc["cmdId"]  | "";
  String status = doc["status"] | "";
  String action = doc["action"] | "";
  if (cmdId == "" || status == "" || action == "") return;
  if (status != "pending") return;
  if (cmdId == lastProcessedCmdId) return;  // schon abgearbeitet

  // is<float>() ist auch für Ganzzahlen true; fehlende Felder → Default.
  int   pump    = doc["pump"].is<float>()    ? (int)doc["pump"].as<float>()   : -1;
  float ml      = doc["ml"].is<float>()      ? doc["ml"].as<float>()          : NAN;
  long  steps   = doc["steps"].is<float>()   ? (long)doc["steps"].as<float>() : 0;
  float phValue = doc["phValue"].is<float>() ? doc["phValue"].as<float>()     : NAN;

  // Stop hat Vorrang und darf IMMER durch — auch während laufender Aktion
  if (action == "stop") {
    lastProcessedCmdId = cmdId;
    abortAll();
    pb_sync::updateCommandStatus("done");
    return;
  }

  // Andere Commands warten bis nichts mehr läuft (bleiben pending)
  if (current.active || phCal.active) return;

  lastProcessedCmdId = cmdId;
  Serial.printf("[Cmd] Pending entdeckt: action=%s id=%s\n", action.c_str(), cmdId.c_str());

  startCommand(cmdId, action, pump, ml, steps, phValue);
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

PlanEntry parseEntry(JsonObjectConst entry, bool isKH) {
  PlanEntry pe = { 0, 0, 0, 0, false, false };
  pe.date = (time_t)(entry["date"] | 0L);
  if (isKH) {
    pe.dosageML = entry["dosageML"] | 0.0f;
  } else {
    pe.caDosageML = entry["caDosageML"] | 0.0f;
    pe.mgDosageML = entry["mgDosageML"] | 0.0f;
  }
  pe.isMaintenanceDose = entry["isMaintenanceDose"] | false;
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

// (getPlanArray entfällt — Plan-Arrays werden unten direkt mit ArduinoJson iteriert)

// ---------- Auto-Dosing-Sequenz prüfen ----------
// Wird alle 30s aufgerufen, fired aber nur in Minute 10-15 einer Dosier-Stunde.
void checkAutoDosingSequence() {
  unsigned long ms = millis();
  if (lastSequenceCheckMs != 0 && ms - lastSequenceCheckMs < SEQUENCE_CHECK_INTERVAL_MS) return;
  lastSequenceCheckMs = ms;

  if (current.active || !queueEmpty()) return;
  if (!settings_cache::autoDosing) return;  // Master-Schalter aus → keine Auto-Doses
  if (isPlanCacheStale()) return;           // kein Plan im Cache

  time_t now; time(&now);
  if (now < 1700000000) return;
  struct tm t;
  localtime_r(&now, &t);

  // Trigger-Fenster: Minute 10-15 in einer Dosier-Stunde (hour % intervalHours == 0)
  if (settings_cache::intervalHours() <= 0) return;
  if (t.tm_hour % settings_cache::intervalHours() != 0 ||
      t.tm_min < 10 || t.tm_min > 15) return;

  JsonDocument doc;
  if (deserializeJson(doc, cachedPlanJson)) return;  // ungültiger Cache → abbrechen

  // Stunden-Fenster für AUSGLEICH-Detection
  time_t hourStart = now - t.tm_min * 60 - t.tm_sec;
  time_t hourEnd = hourStart + 3600 - 1;

  // ---------- KH-Plan analysieren ----------
  JsonArrayConst khArr = doc["khEntries"].as<JsonArrayConst>();
  float khDosageML = 0;
  bool khAdjustment = false;
  if (!khArr.isNull()) {
    // 1) Spezifische AUSGLEICH-Dose für diese Stunde?
    for (JsonObjectConst e : khArr) {
      PlanEntry pe = parseEntry(e, true);
      if (pe.isMaintenanceDose || pe.date == 0) continue;
      if (pe.date >= hourStart && pe.date <= hourEnd) {
        khDosageML = pe.dosageML;
        khAdjustment = true;
        break;
      }
    }
    // 2) ERHALTUNG (nur wenn kein Ausgleich für diese Stunde)
    if (!khAdjustment) {
      for (JsonObjectConst e : khArr) {
        PlanEntry pe = parseEntry(e, true);
        if (pe.isMaintenanceDose || pe.date == 0) {
          khDosageML = pe.dosageML;
          break;
        }
      }
    }
  }

  // ---------- Ca-Plan analysieren ----------
  JsonArrayConst caArr = doc["caEntries"].as<JsonArrayConst>();
  float caDosageML = 0, mgDosageML = 0;
  bool caAdjustment = false;
  if (!caArr.isNull()) {
    for (JsonObjectConst e : caArr) {
      PlanEntry pe = parseEntry(e, false);
      if (pe.isMaintenanceDose || pe.date == 0) continue;
      if (pe.date >= hourStart && pe.date <= hourEnd) {
        caDosageML = pe.caDosageML;
        mgDosageML = pe.mgDosageML;
        caAdjustment = true;
        break;
      }
    }
    if (!caAdjustment) {
      for (JsonObjectConst e : caArr) {
        PlanEntry pe = parseEntry(e, false);
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
  // settings_cache::loadFromNVS() wird jetzt schon in setup() VOR pumps::begin()
  // aufgerufen — hier nichts mehr nötig.
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

// Einmalig nach PocketBase-Login: pH-Kalibrierung aus PocketBase übernehmen,
// falls lokal (NVS) keine vorhanden ist. Deckt ESP-Tausch + JSON-Restore ab.
void syncPhCalibrationOnce() {
  static bool done = false;
  if (done || ph_sensor::isCalibrated() || !pb_sync::isReady()) return;
  done = true;
  float v4 = NAN, v7 = NAN; bool cal = false;
  if (pb_sync::fetchPhCalibration(v4, v7, cal) && cal) {
    ph_sensor::setCalibration(v4, v7);
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putFloat("phV4", v4);
    p.putFloat("phV7", v7);
    p.end();
    Serial.printf("[pH] Kalibrierung aus PocketBase übernommen: V4=%.4f V7=%.4f\n", v4, v7);
  }
}

void tick() {
  checkPumpFinished();
  tickPhCalibration();             // pH-Kalibrierungs-Sampling
  pollCommands();
  syncPumpConfigs();
  settings_cache::sync();
  syncPlan();
  syncPhCalibrationOnce();         // pH-Kalib aus Cloud falls NVS leer
  checkPhSampleSchedule();         // pH-Probe um :05 schreiben
  checkAutoDosingSequence();       // Dosier-Sequenz um :10
  pb_sync::flushBuffer();    // Backlog-Uploads versuchen
}

} // namespace plan_executor
