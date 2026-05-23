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
String pathCommands()  { return "users/" + currentUid + "/aquarium-commands"; }
String pathPump(int i) { return "users/" + currentUid + "/aquarium/pump-" + String(i); }
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

// ---------- Pump-Konfig lesen (Kalibrierung) ----------
// Returns true wenn erfolgreich, schreibt mlPerStep ins out-Parameter.
bool fetchPumpMlPerStep(int pumpIdx, float &outMlPerStep) {
  if (!isReady()) return false;
  if (!Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", pathPump(pumpIdx).c_str())) {
    return false;
  }
  FirebaseJson doc;
  doc.setJsonData(fbdo.payload());
  FirebaseJsonData v;
  if (doc.get(v, "fields/mlPerStep/doubleValue") && v.success) {
    outMlPerStep = v.floatValue;
    return true;
  }
  // Manchmal als integer gespeichert (0)
  if (doc.get(v, "fields/mlPerStep/integerValue") && v.success) {
    outMlPerStep = (float)v.intValue;
    return true;
  }
  return false;
}

// ---------- Command-Queue lesen ----------
// Liefert via runQuery alle pending commands. Bis zu 5 werden zurückgegeben.
// out: FirebaseJsonArray mit den Doc-Inhalten (jeweils mit "name" für ID-Extraktion)
bool fetchPendingCommands(FirebaseJsonArray &out) {
  out.clear();
  if (!isReady()) return false;
  // StructuredQuery: SELECT * FROM aquarium-commands WHERE status == "pending"
  // Mobizt's Library: Firebase.Firestore.runQuery
  FirebaseJson queryJson;
  queryJson.set("structuredQuery/from/[0]/collectionId", "aquarium-commands");
  queryJson.set("structuredQuery/where/fieldFilter/field/fieldPath", "status");
  queryJson.set("structuredQuery/where/fieldFilter/op", "EQUAL");
  queryJson.set("structuredQuery/where/fieldFilter/value/stringValue", "pending");
  queryJson.set("structuredQuery/limit", 5);
  // OrderBy createdAt asc (älteste zuerst)
  queryJson.set("structuredQuery/orderBy/[0]/field/fieldPath", "createdAt");
  queryJson.set("structuredQuery/orderBy/[0]/direction", "ASCENDING");

  // Parent für runQuery: users/{uid}
  String parent = "users/" + currentUid;
  if (!Firebase.Firestore.runQuery(&fbdo, FIREBASE_PROJECT_ID, "", parent.c_str(), &queryJson)) {
    return false;
  }
  // fbdo.payload() ist Array von [{document: {...}, ...}, ...]
  FirebaseJsonArray arr;
  arr.setJsonArrayData(fbdo.payload());
  // Jedes Element kann ein Result mit document sein oder {} (Sentinel)
  for (size_t i = 0; i < arr.size(); i++) {
    FirebaseJsonData item;
    arr.get(item, i);
    FirebaseJson el;
    el.setJsonData(item.stringValue);
    FirebaseJsonData docPath;
    if (el.get(docPath, "document/name") && docPath.success) {
      out.add(el);
    }
  }
  return true;
}

// ---------- Command-Status aktualisieren ----------
// Schreibt status + optional result-Felder via PATCH.
// commandPath: vollständiger Pfad (so wie er aus dem document/name kommt, ohne
// das "projects/.../documents/" Prefix)
bool updateCommand(const String &commandPath, const String &status, FirebaseJson *resultJson = nullptr) {
  if (!isReady()) return false;
  FirebaseJson content;
  content.set("fields/status/stringValue", status);
  time_t now; time(&now);
  if (now > 1700000000) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    if (status == "running") content.set("fields/startedAt/timestampValue", buf);
    else if (status == "done" || status == "failed") content.set("fields/completedAt/timestampValue", buf);
  }
  String mask = "status";
  if (status == "running") mask += ",startedAt";
  if (status == "done" || status == "failed") mask += ",completedAt";

  if (resultJson) {
    // result ist ein Map-Field. Wir setzen es als komplettes Objekt.
    content.set("fields/result/mapValue/fields", *resultJson);
    mask += ",result";
  }
  return Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", commandPath.c_str(),
                                          content.raw(), mask.c_str());
}

} // namespace firebase_sync
