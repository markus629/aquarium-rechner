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
void syncPumpConfigs() {
  unsigned long now = millis();
  if (lastPumpConfigSyncMs != 0 && now - lastPumpConfigSyncMs < 5 * 60 * 1000UL) return;
  lastPumpConfigSyncMs = now;
  for (int i = 0; i < pumps::NUM_PUMPS; i++) {
    float v;
    if (firebase_sync::fetchPumpMlPerStep(i, v)) {
      pumps::mlPerStep[i] = v;
      Serial.printf("[PumpConfig] Pumpe %d: mlPerStep=%.5f\n", i, v);
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

// ---------- Plan-Check (alle 60 s) ----------
void checkPlanForDueDose() {
  unsigned long now = millis();
  if (lastPlanCheckMs != 0 && now - lastPlanCheckMs < PLAN_CHECK_INTERVAL_MS) return;
  lastPlanCheckMs = now;
  // TODO Session C: Plan aus Firestore (oder Cache) holen, fällige Einträge ausführen
}

// ---------- Pump-Status checken (im Loop aufrufen) ----------
void checkPumpFinished() {
  int finished = pumps::checkAndDisable();
  if (finished >= 0 && current.active) {
    finishCommand(true);
  }
}

void tick() {
  checkPumpFinished();
  pollCommands();
  checkPlanForDueDose();
  syncPumpConfigs();
}

} // namespace plan_executor
