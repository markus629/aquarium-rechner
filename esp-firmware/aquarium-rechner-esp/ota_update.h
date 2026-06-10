// =============================================================
// OTA-Update — holt Firmware aus GitHub Releases (public)
// =============================================================
// Workflow:
// 1. GET https://api.github.com/repos/<OWNER>/<REPO>/releases/latest
//    Liefert JSON mit { "tag_name": "v1.0.4", "assets": [{ "name": "...", "browser_download_url": "..." }] }
// 2. Tag-Name parsen (führendes "v" entfernen), mit FW_VERSION vergleichen
// 3. Asset finden das OTA_ASSET_FILTER enthält
// 4. HTTPUpdate auf die download-URL → automatischer Reboot
//
// Sicherheit:
// - Master-Schalter `settings_cache::otaAutoUpdate` (default: aus)
// - Time-Gate: NIE zwischen :08 und :20 jeder geraden Stunde
//   (Sicherheits-Puffer um den Dosier-Trigger :10–:15 herum)
// - Manuelle Auslösung über Command (action="otaCheck")
// =============================================================
#pragma once

#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "config.h"
#include "settings_cache.h"

namespace ota_update {

unsigned long lastCheckMs = 0;
bool inProgress = false;
String availableVersion = "";   // letzter erkannter Release (für Heartbeat/UI)
String availableUrl     = "";

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

// Schutzfenster: NICHT zwischen :05 und :20 in einer Dosier-Stunde aktualisieren.
// Dosier-Stunde = jede Stunde wo (hour % intervalHours == 0).
// Bei 12×/Tag → gerade Stunden, bei 6×/Tag → 0,4,8,12,16,20, etc.
// pH-Sample feuert in :05-:08, Dosier-Sequenz in :10-:15 — Reboot in dem
// Zeitraum würde sie killen.
bool isProtectedWindow() {
  time_t now; time(&now);
  if (now < 1700000000) return true;  // Zeit noch nicht synced → safer not to update
  struct tm t; localtime_r(&now, &t);
  int intH = settings_cache::intervalHours();
  if (intH <= 0) return true;  // ungültig → konservativ schützen
  if (t.tm_hour % intH == 0 && t.tm_min >= 5 && t.tm_min <= 20) return true;
  return false;
}

void performUpdate(const String &url) {
  if (url.length() == 0) { Serial.println("[OTA] keine URL"); return; }
  Serial.printf("[OTA] Lade Firmware: %s\n", url.c_str());
  inProgress = true;
  WiFiClientSecure client;
  client.setInsecure();   // GitHub hat valides Cert, aber wir sparen uns das Root-Cert-Pinning hier
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

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
      break;
  }
  inProgress = false;
}

// Holt GitHub /releases/latest, parst, setzt availableVersion + availableUrl.
// Returns true wenn ein gültiges Release gefunden wurde (auch wenn gleiche Version).
bool fetchLatestRelease() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String("https://api.github.com/repos/") + OTA_GITHUB_OWNER + "/" + OTA_GITHUB_REPO + "/releases/latest";
  http.useHTTP10(true);    // wichtig für ArduinoJson stream
  http.addHeader("User-Agent", FW_NAME);
  http.addHeader("Accept", "application/vnd.github+json");
  if (!http.begin(client, url)) { Serial.println("[OTA] http.begin failed"); return false; }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] GitHub API HTTP %d\n", code);
    http.end();
    return false;
  }

  // Stream-Parsing: nur tag_name + assets[*].{name,browser_download_url}
  // (ArduinoJson-v7-API: JsonDocument wächst dynamisch, kein Size-Template)
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[OTA] JSON-Fehler: %s\n", err.c_str()); return false; }

  const char* tag = doc["tag_name"] | "";
  if (!tag || strlen(tag) == 0) return false;

  // Führendes "v" entfernen falls vorhanden
  String version = (tag[0] == 'v' || tag[0] == 'V') ? String(tag + 1) : String(tag);

  String foundUrl = "";
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    const char* name = asset["name"] | "";
    const char* dl   = asset["browser_download_url"] | "";
    if (name && strstr(name, OTA_ASSET_FILTER) != nullptr) {
      foundUrl = String(dl);
      break;
    }
  }
  if (foundUrl.length() == 0) {
    Serial.printf("[OTA] Release %s hat kein passendes Asset (Filter: %s)\n", version.c_str(), OTA_ASSET_FILTER);
    return false;
  }
  availableVersion = version;
  availableUrl = foundUrl;
  Serial.printf("[OTA] GitHub latest: %s\n", version.c_str());
  return true;
}

// Prüft + updated wenn neuere Version verfügbar.
// forceCheck überspringt Auto-Update-Gate und Intervall (manueller Button).
// Returns: true wenn ein vollständiger Check (inkl. Schutzfenster-OK) durchlief,
//          false wenn das Schutzfenster getriggert hat (= bitte bald nochmal versuchen).
bool checkAndUpdate(bool forceCheck = false) {
  if (inProgress) return true;
  if (!forceCheck && !settings_cache::otaAutoUpdate) return true;  // Auto aus → nur Manual

  if (!fetchLatestRelease()) return true;  // Netz-Fehler → erst beim nächsten Tick erneut
  if (compareVersions(FW_VERSION, availableVersion) >= 0) {
    Serial.printf("[OTA] keine neuere Version (lokal %s, remote %s)\n", FW_VERSION, availableVersion.c_str());
    return true;
  }
  if (isProtectedWindow()) {
    Serial.println("[OTA] Schutzfenster (Dosier-Zeit) — Update verschoben, retry in ~25 Min");
    return false;
  }
  // NIE rebooten während eine Pumpe läuft (auch manuelle Doses/Kalibrierungen
  // außerhalb des Plan-Schutzfensters!)
  if (pumps::isBusy() || pumps::ds.phase != pumps::PHASE_IDLE) {
    Serial.println("[OTA] Pumpe aktiv — Update verschoben, retry in ~25 Min");
    return false;
  }
  Serial.printf("[OTA] Neue Version %s → starte Update\n", availableVersion.c_str());
  performUpdate(availableUrl);
  return true;
}

void tick() {
  unsigned long now = millis();
  if (now - lastCheckMs < OTA_CHECK_INTERVAL_MS && lastCheckMs != 0) return;
  bool fullCheck = checkAndUpdate(false);
  if (fullCheck) {
    lastCheckMs = now;   // normal: warte 6 h bis nächsten Check
  } else {
    // Schutzfenster aktiv — in ~25 Min nochmal probieren (dann sicher außerhalb)
    lastCheckMs = now - (OTA_CHECK_INTERVAL_MS - 25UL * 60UL * 1000UL);
  }
}

} // namespace ota_update
