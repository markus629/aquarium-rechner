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
#include "upload_buffer.h"

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
// Erweiterter Heartbeat mit Statistiken und Pumpen-Kalibrierstatus.
struct HeartbeatStats {
  unsigned long dosesTotal;
  unsigned long dosesFailedTotal;
  int dosesOk24h;
  int dosesFail24h;
  bool pumpsCalibrated[4];  // mlPerStep > 0 ?
  int bufferQueueSize;       // wie viele Uploads noch in Queue
};

bool sendHeartbeat(float phValue, int phSamples, long uptimeSec, const HeartbeatStats &stats) {
  if (!isReady()) return false;
  FirebaseJson content;
  content.set("fields/online/booleanValue", true);
  content.set("fields/firmware/stringValue", FW_VERSION);
  time_t now;
  time(&now);
  if (now > 1700000000) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    content.set("fields/lastSeen/timestampValue", buf);
  }
  content.set("fields/freeHeap/integerValue", (int)ESP.getFreeHeap());
  content.set("fields/rssi/integerValue", (int)WiFi.RSSI());
  content.set("fields/ip/stringValue", WiFi.localIP().toString().c_str());
  content.set("fields/uptimeSec/integerValue", (int)uptimeSec);
  if (!isnan(phValue)) {
    content.set("fields/lastPh/doubleValue", phValue);
    content.set("fields/lastPhSamples/integerValue", phSamples);
  }
  // Statistiken
  content.set("fields/dosesTotal/integerValue", (int)stats.dosesTotal);
  content.set("fields/dosesFailedTotal/integerValue", (int)stats.dosesFailedTotal);
  content.set("fields/dosesOk24h/integerValue", stats.dosesOk24h);
  content.set("fields/dosesFail24h/integerValue", stats.dosesFail24h);
  content.set("fields/bufferQueueSize/integerValue", stats.bufferQueueSize);
  // Pumpen-Kalibrier-Array
  for (int i = 0; i < 4; i++) {
    String key = String("fields/pumpsCalibrated/arrayValue/values/[") + i + "]/booleanValue";
    content.set(key.c_str(), stats.pumpsCalibrated[i]);
  }

  String mask = "online,firmware,lastSeen,freeHeap,rssi,ip,uptimeSec,"
                "lastPh,lastPhSamples,dosesTotal,dosesFailedTotal,"
                "dosesOk24h,dosesFail24h,bufferQueueSize,pumpsCalibrated";
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

// ---------- pH-Messung schreiben (gepuffert) ----------
// Immer erst in upload_buffer einreihen, der Flush-Mechanismus
// versucht periodisch zu senden. Garantiert: kein Datenverlust.
void addPhMeasurement(float phValue) {
  FirebaseJson content;
  content.set("fields/type/stringValue", "ph");
  content.set("fields/value/doubleValue", phValue);
  content.set("fields/source/stringValue", "esp");
  time_t now; time(&now);
  if (now > 1700000000) content.set("fields/timestamp/integerValue", (int)now);
  upload_buffer::enqueue(pathMeasurements(), content.raw());
}

// ---------- Dosier-Bestätigung schreiben (gepuffert) ----------
void addDosing(int pumpIdx, float ml, const char *dosageType, bool isAutomatic, float factor, bool success) {
  FirebaseJson content;
  content.set("fields/pump/integerValue", pumpIdx);
  content.set("fields/ml/doubleValue", ml);
  content.set("fields/dosageType/stringValue", dosageType);
  content.set("fields/isAutomatic/booleanValue", isAutomatic);
  content.set("fields/factor/doubleValue", factor);
  content.set("fields/success/booleanValue", success);
  content.set("fields/source/stringValue", "esp");
  time_t now; time(&now);
  if (now > 1700000000) content.set("fields/timestamp/integerValue", (int)now);
  upload_buffer::enqueue(pathDosings(), content.raw());
}

// ---------- Container-Level dekrementieren ----------
// Schreibt nur das eine Array-Feld zurück (PATCH mit mask).
// ESP hat lokal keinen Cache der Levels — wir holen sie kurz aus Firestore.
// Falls offline: Web macht das eh auch separat bei Re-Sync.
void decrementContainerLevel(int pumpIdx, float ml) {
  if (!isReady() || pumpIdx < 0 || pumpIdx > 3 || ml <= 0) return;
  // Settings-Doc holen
  String settingsPath = "users/" + currentUid + "/aquarium/settings";
  if (!Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", settingsPath.c_str())) return;
  FirebaseJson doc;
  doc.setJsonData(fbdo.payload());
  // Aktuelle Levels lesen
  float levels[4] = { 5000, 5000, 5000, 5000 };
  for (int i = 0; i < 4; i++) {
    FirebaseJsonData v;
    String path = String("fields/containerLevel/arrayValue/values/[") + i + "]/doubleValue";
    if (doc.get(v, path.c_str()) && v.success) levels[i] = v.floatValue;
    else {
      String pathInt = String("fields/containerLevel/arrayValue/values/[") + i + "]/integerValue";
      if (doc.get(v, pathInt.c_str()) && v.success) levels[i] = (float)v.intValue;
    }
  }
  // Dekrementieren
  levels[pumpIdx] -= ml;
  if (levels[pumpIdx] < 0) levels[pumpIdx] = 0;
  // Zurückschreiben via PATCH mit mask
  FirebaseJson content;
  for (int i = 0; i < 4; i++) {
    String key = String("fields/containerLevel/arrayValue/values/[") + i + "]/doubleValue";
    content.set(key.c_str(), levels[i]);
  }
  Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", settingsPath.c_str(),
                                   content.raw(), "containerLevel");
}

// ---------- pH-Kalibrierung schreiben (gepuffert) ----------
void writePhCalibration(float voltage_pH4, float voltage_pH7, bool isCalibrated) {
  FirebaseJson content;
  if (!isnan(voltage_pH4)) content.set("fields/voltage_pH4/doubleValue", voltage_pH4);
  if (!isnan(voltage_pH7)) content.set("fields/voltage_pH7/doubleValue", voltage_pH7);
  content.set("fields/isCalibrated/booleanValue", isCalibrated);
  time_t now; time(&now);
  if (now > 1700000000) content.set("fields/lastCalibratedAt/integerValue", (int)now);
  String path = "users/" + currentUid + "/aquarium/ph-calibration";
  upload_buffer::enqueue(path, content.raw());
}

// ---------- pH-Kalibrierung lesen ----------
bool fetchPhCalibration(float &v4_out, float &v7_out, bool &calibrated_out) {
  if (!isReady()) return false;
  String path = "users/" + currentUid + "/aquarium/ph-calibration";
  if (!Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", path.c_str())) return false;
  FirebaseJson doc;
  doc.setJsonData(fbdo.payload());
  FirebaseJsonData v;
  if (doc.get(v, "fields/voltage_pH4/doubleValue") && v.success) v4_out = v.floatValue;
  if (doc.get(v, "fields/voltage_pH7/doubleValue") && v.success) v7_out = v.floatValue;
  calibrated_out = !isnan(v4_out) && !isnan(v7_out);
  return true;
}

// ---------- Buffer-Flush ----------
// Versucht den ältesten gepufferten Eintrag zu senden.
// Returns true wenn erfolgreich (oder Queue leer), false wenn fehlgeschlagen.
bool flushBuffer() {
  if (!isReady() || upload_buffer::queue.empty()) return true;
  unsigned long ms = millis();
  if (upload_buffer::nextRetryAtMs != 0 && ms < upload_buffer::nextRetryAtMs) return false;
  if (ms - upload_buffer::lastFlushAttemptMs < upload_buffer::FLUSH_INTERVAL_MS) return false;
  upload_buffer::lastFlushAttemptMs = ms;

  upload_buffer::PendingWrite &first = upload_buffer::queue.front();
  first.attempts++;
  bool ok;
  // Heuristik: ph-calibration ist ein einzelnes Doc → patchDocument (Merge)
  if (first.path.endsWith("/ph-calibration")) {
    ok = Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "",
                                          first.path.c_str(), first.payload.c_str(),
                                          "voltage_pH4,voltage_pH7,isCalibrated,lastCalibratedAt");
  } else {
    // measurements/items oder dosings/items → createDocument
    ok = Firebase.Firestore.createDocument(&fbdo, FIREBASE_PROJECT_ID, "",
                                           first.path.c_str(), "",
                                           first.payload.c_str(), "");
  }
  if (ok) {
    Serial.printf("[Buffer] flushed (%d remaining)\n", (int)upload_buffer::queue.size() - 1);
    upload_buffer::queue.erase(upload_buffer::queue.begin());
    upload_buffer::save();
    upload_buffer::nextRetryAtMs = 0;
    return true;
  } else {
    Serial.printf("[Buffer] flush FAIL (attempts=%d, %d in queue): %s\n",
                  first.attempts, (int)upload_buffer::queue.size(),
                  fbdo.errorReason().c_str());
    upload_buffer::nextRetryAtMs = ms + upload_buffer::BACKOFF_AFTER_FAIL_MS;
    upload_buffer::save();  // attempts++ persistieren
    return false;
  }
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
// KEIN orderBy — vermeidet Composite-Index-Requirement.
bool fetchPendingCommands(FirebaseJsonArray &out) {
  out.clear();
  if (!isReady()) return false;
  FirebaseJson queryJson;
  queryJson.set("structuredQuery/from/[0]/collectionId", "aquarium-commands");
  queryJson.set("structuredQuery/where/fieldFilter/field/fieldPath", "status");
  queryJson.set("structuredQuery/where/fieldFilter/op", "EQUAL");
  queryJson.set("structuredQuery/where/fieldFilter/value/stringValue", "pending");
  queryJson.set("structuredQuery/limit", 5);

  // Parent: users/{uid} (Subcollection drunter)
  String parent = "users/" + currentUid;
  if (!Firebase.Firestore.runQuery(&fbdo, FIREBASE_PROJECT_ID, "", parent.c_str(), &queryJson)) {
    Serial.printf("[Cmd] runQuery FAIL: %s | payload: %s\n",
                  fbdo.errorReason().c_str(), fbdo.payload().c_str());
    return false;
  }
  // fbdo.payload() ist Array [{document:{...}}, ...]
  FirebaseJsonArray arr;
  arr.setJsonArrayData(fbdo.payload());
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
  if (out.size() > 0) Serial.printf("[Cmd] %d pending commands\n", (int)out.size());
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
