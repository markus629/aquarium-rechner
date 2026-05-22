// =============================================================
// Firebase-Anbindung — Auth + Firestore-Reads/Writes
// =============================================================
// Verwendet die "Firebase ESP Client" Library von Mobizt:
// https://github.com/mobizt/Firebase-ESP-Client
//
// Auth-Flow:
//   - Login mit E-Mail + Passwort (vom Setup-Portal)
//   - Library refresht Tokens automatisch
//
// Firestore-Pfade:
//   users/{uid}/aquarium/info        ← Heartbeat (write)
//   users/{uid}/aquarium/settings    ← Settings (read)
//   users/{uid}/aquarium/plan-current ← Aktueller Plan (read)
//   users/{uid}/aquarium/commands    ← Command-Queue (read+update)
//   users/{uid}/aquarium/measurements/items/{auto-id} ← pH-Werte (write)
//   users/{uid}/aquarium/dosings/items/{auto-id}      ← Dose-Bestätigungen (write)
// =============================================================
#pragma once

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <ArduinoJson.h>
#include "config.h"

namespace firebase_sync {

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig fbConfig;
bool ready = false;
String currentUid;

// ---------- Init + Login ----------
bool begin(const String &email, const String &password) {
  fbConfig.api_key = FIREBASE_API_KEY;
  auth.user.email = email.c_str();
  auth.user.password = password.c_str();
  fbConfig.token_status_callback = tokenStatusCallback;

  Firebase.begin(&fbConfig, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setBSSLBufferSize(4096, 1024);  // mehr Buffer für TLS

  Serial.println("[Firebase] Warte auf Authentifizierung …");
  unsigned long start = millis();
  while (!Firebase.ready() && (millis() - start) < 30000) {
    delay(250);
  }
  if (!Firebase.ready()) {
    Serial.println("[Firebase] FEHLER: Auth-Timeout");
    return false;
  }
  currentUid = auth.token.uid.c_str();
  Serial.printf("[Firebase] eingeloggt als uid=%s\n", currentUid.c_str());
  ready = true;
  return true;
}

bool isReady() { return ready && Firebase.ready(); }
const String& uid() { return currentUid; }

// ---------- Pfad-Builder ----------
String pathInfo()      { return "users/" + currentUid + "/aquarium/info"; }
String pathSettings()  { return "users/" + currentUid + "/aquarium/settings"; }
String pathPlan()      { return "users/" + currentUid + "/aquarium/plan-current"; }
String pathCommands()  { return "users/" + currentUid + "/aquarium/commands"; }
String pathMeasurements() { return "users/" + currentUid + "/aquarium/measurements/items"; }
String pathDosings()   { return "users/" + currentUid + "/aquarium/dosings/items"; }

// ---------- Heartbeat schreiben ----------
bool sendHeartbeat(float phValue, int phSamples, long uptimeSec, long pendingDoses) {
  if (!isReady()) return false;
  FirebaseJson content;
  content.set("fields/online/booleanValue", true);
  content.set("fields/firmware/stringValue", FW_VERSION);
  content.set("fields/lastSeen/timestampValue", "");  // serverTimestamp wird nicht direkt unterstützt → wir setzen es manuell
  // Wir umgehen serverTimestamp: speichern UTC ISO-String von ESP-Uhr.
  // Genauigkeit reicht für lastSeen-Anzeige.
  time_t now;
  time(&now);
  if (now > 1700000000) {  // Plausibility-Check
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    content.set("fields/lastSeen/timestampValue", buf);
  }
  content.set("fields/freeHeap/integerValue", (int)ESP.getFreeHeap());
  content.set("fields/rssi/integerValue", (int)WiFi.RSSI());
  content.set("fields/ip/stringValue", WiFi.localIP().toString().c_str());
  content.set("fields/uptimeSec/integerValue", (int)uptimeSec);
  content.set("fields/pendingDoses/integerValue", (int)pendingDoses);
  if (!isnan(phValue)) {
    content.set("fields/lastPh/doubleValue", phValue);
    content.set("fields/lastPhSamples/integerValue", phSamples);
  }

  // PATCH (Merge) — vorhandene Felder bleiben erhalten
  String mask = "online,firmware,lastSeen,freeHeap,rssi,ip,uptimeSec,pendingDoses,lastPh,lastPhSamples";
  return Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", pathInfo().c_str(),
                                          content.raw(), mask.c_str());
}

// ---------- Settings lesen ----------
// Returns true wenn Daten geladen, schreibt JSON in `out`
bool fetchSettings(FirebaseJson &out) {
  if (!isReady()) return false;
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", pathSettings().c_str())) {
    out.setJsonData(fbdo.payload());
    return true;
  }
  return false;
}

// ---------- Plan lesen ----------
bool fetchPlan(FirebaseJson &out) {
  if (!isReady()) return false;
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", pathPlan().c_str())) {
    out.setJsonData(fbdo.payload());
    return true;
  }
  return false;
}

// ---------- pH-Messung schreiben ----------
bool addPhMeasurement(float phValue) {
  if (!isReady()) return false;
  FirebaseJson content;
  content.set("fields/type/stringValue", "ph");
  content.set("fields/value/doubleValue", phValue);
  content.set("fields/source/stringValue", "esp");
  time_t now;
  time(&now);
  if (now > 1700000000) {
    content.set("fields/timestamp/integerValue", (int)now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    content.set("fields/createdAt/timestampValue", buf);
  }
  return Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "",
                                           pathMeasurements().c_str(), "",
                                           content.raw(), "");
}

// ---------- Dosier-Bestätigung schreiben ----------
bool addDosing(int pumpIdx, float ml, const char *dosageType, bool isAutomatic, float factor, bool success) {
  if (!isReady()) return false;
  FirebaseJson content;
  content.set("fields/pump/integerValue", pumpIdx);
  content.set("fields/ml/doubleValue", ml);
  content.set("fields/dosageType/stringValue", dosageType);
  content.set("fields/isAutomatic/booleanValue", isAutomatic);
  content.set("fields/factor/doubleValue", factor);
  content.set("fields/success/booleanValue", success);
  content.set("fields/source/stringValue", "esp");
  time_t now;
  time(&now);
  if (now > 1700000000) {
    content.set("fields/timestamp/integerValue", (int)now);
  }
  return Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "",
                                           pathDosings().c_str(), "",
                                           content.raw(), "");
}

// ---------- Command-Queue lesen ----------
// Pollt das commands-Dokument, gibt Liste offener Commands als JSON zurück.
bool fetchPendingCommands(FirebaseJson &out) {
  if (!isReady()) return false;
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", pathCommands().c_str())) {
    out.setJsonData(fbdo.payload());
    return true;
  }
  return false;
}

// ---------- Command als erledigt markieren ----------
// Setzt status="done" + result-Felder im Command-Dokument.
bool ackCommand(const String &commandId, const String &resultJson) {
  if (!isReady()) return false;
  // Pfad: users/{uid}/aquarium/commands/{commandId}
  String path = pathCommands() + "/" + commandId;
  FirebaseJson content;
  content.setJsonData(resultJson);
  String mask = "status,result,completedAt";
  return Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", path.c_str(),
                                          content.raw(), mask.c_str());
}

} // namespace firebase_sync
