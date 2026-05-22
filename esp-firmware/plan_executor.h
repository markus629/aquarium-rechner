// =============================================================
// Plan-Executor — führt Dosierpläne und Commands aus
// =============================================================
// Aufgaben:
// 1. Plan aus Firestore holen + im NVS cachen (Offline-Fallback)
// 2. Pro Minute prüfen ob ein Plan-Eintrag jetzt fällig ist
// 3. KH-Tag/Nacht-Entscheidung lokal (anhand pH oder Uhrzeit)
// 4. Dose ausführen via pumps::runMl, dann Bestätigung in Firestore
// 5. Commands (Kalibrierung, manuelle Dosis) ausführen
// =============================================================
#pragma once

#include <ArduinoJson.h>
#include "config.h"
#include "firebase_sync.h"
#include "pumps.h"
#include "ph_sensor.h"

namespace plan_executor {

unsigned long lastPlanCheckMs = 0;
unsigned long lastCommandPollMs = 0;
bool hasPendingCommand = false;

// ---------- Command-Polling (Adaptive: schneller wenn was offen) ----------
void pollCommands() {
  unsigned long now = millis();
  unsigned long interval = hasPendingCommand ? COMMAND_POLL_FAST_MS : COMMAND_POLL_NORMAL_MS;
  if (now - lastCommandPollMs < interval) return;
  lastCommandPollMs = now;

  FirebaseJson doc;
  if (!firebase_sync::fetchPendingCommands(doc)) return;

  // commands-Doc hat ein Array "queue" mit Command-Objekten
  // Format: { queue: [{ id, action, pump, ml?, steps?, status: "pending"|"running"|"done"|"failed", ... }] }
  FirebaseJsonData arr;
  doc.get(arr, "fields/queue/arrayValue/values");
  if (!arr.success) {
    hasPendingCommand = false;
    return;
  }
  FirebaseJsonArray queue;
  arr.get(queue);
  hasPendingCommand = false;
  for (size_t i = 0; i < queue.size(); i++) {
    FirebaseJsonData item;
    queue.get(item, i);
    FirebaseJson cmd;
    cmd.setJsonData(item.stringValue);
    FirebaseJsonData statusF, actionF;
    cmd.get(statusF, "mapValue/fields/status/stringValue");
    cmd.get(actionF, "mapValue/fields/action/stringValue");
    if (!statusF.success || statusF.stringValue != "pending") continue;
    hasPendingCommand = true;
    // TODO: dispatch action
    Serial.printf("[Plan] Command-Action: %s (noch nicht implementiert)\n", actionF.stringValue.c_str());
    // Next session: hier Kalibrierung / Manuelle Dose / etc. ausführen
    break;
  }
}

// ---------- Plan-Check (alle 60 s) ----------
void checkPlanForDueDose() {
  unsigned long now = millis();
  if (now - lastPlanCheckMs < PLAN_CHECK_INTERVAL_MS && lastPlanCheckMs != 0) return;
  lastPlanCheckMs = now;
  // TODO: Plan aus Firestore (oder Cache) holen, fällige Einträge ausführen
  // Wird in der nächsten Session vollständig implementiert
}

void tick() {
  pollCommands();
  checkPlanForDueDose();
}

} // namespace plan_executor
