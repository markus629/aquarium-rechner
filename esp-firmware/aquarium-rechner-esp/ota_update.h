// =============================================================
// OTA-Update — holt Firmware aus Firebase Storage
// =============================================================
// Workflow:
// 1. Lese users/{uid}/aquarium/firmware-latest:
//    { version: "1.0.3", url: "https://firebasestorage.googleapis.com/.../app.bin?...", sha256?: "..." }
// 2. Vergleiche mit FW_VERSION
// 3. Bei neuerer Version: HTTPUpdate auf url → automatischer Reboot in neue Firmware
//
// Die Storage-URL muss public-lesbar sein ODER einen
// access-token enthalten. Wir verwenden Firebase Storage download-URL
// die das Token bereits enthält → kein Auth-Header nötig für den Download.
// =============================================================
#pragma once

#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "config.h"
#include "firebase_sync.h"

namespace ota_update {

unsigned long lastCheckMs = 0;
bool inProgress = false;

// SemVer-Vergleich: 1.2.3 vs 1.2.4 → -1 (need update)
int compareVersions(const String &a, const String &b) {
  int aMaj=0, aMin=0, aPat=0;
  int bMaj=0, bMin=0, bPat=0;
  sscanf(a.c_str(), "%d.%d.%d", &aMaj, &aMin, &aPat);
  sscanf(b.c_str(), "%d.%d.%d", &bMaj, &bMin, &bPat);
  if (aMaj != bMaj) return aMaj < bMaj ? -1 : 1;
  if (aMin != bMin) return aMin < bMin ? -1 : 1;
  if (aPat != bPat) return aPat < bPat ? -1 : 1;
  return 0;
}

void performUpdate(const String &url) {
  Serial.printf("[OTA] Lade Firmware von %s\n", url.c_str());
  inProgress = true;
  WiFiClientSecure client;
  client.setInsecure();  // Firebase Storage hat valides Cert, aber wir sparen uns das Root-Cert hier
  httpUpdate.setLedPin(LED_BUILTIN, LOW);

  t_httpUpdate_return ret = httpUpdate.update(client, url);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] FAILED (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("[OTA] keine Updates");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("[OTA] OK — Reboot");
      // restart erfolgt automatisch
      break;
  }
  inProgress = false;
}

void checkAndUpdate() {
  if (inProgress) return;
  if (!firebase_sync::isReady()) return;

  // firmware-latest aus Firestore lesen
  FirebaseJson doc;
  String path = "users/" + firebase_sync::uid() + "/aquarium/firmware-latest";
  if (!Firebase.Firestore.getDocument(&firebase_sync::fbdo, FIREBASE_PROJECT_ID, "", path.c_str())) {
    // kein firmware-latest Doc — okay, einfach nichts tun
    return;
  }
  doc.setJsonData(firebase_sync::fbdo.payload());
  FirebaseJsonData v, u;
  doc.get(v, "fields/version/stringValue");
  doc.get(u, "fields/url/stringValue");
  if (!v.success || !u.success) return;

  String latest = v.stringValue;
  String url = u.stringValue;
  Serial.printf("[OTA] Aktuell: %s, Verfügbar: %s\n", FW_VERSION, latest.c_str());
  if (compareVersions(FW_VERSION, latest) < 0) {
    Serial.println("[OTA] Neue Version verfügbar — starte Update");
    performUpdate(url);
  }
}

void tick() {
  unsigned long now = millis();
  if (now - lastCheckMs < OTA_CHECK_INTERVAL_MS && lastCheckMs != 0) return;
  lastCheckMs = now;
  checkAndUpdate();
}

} // namespace ota_update
