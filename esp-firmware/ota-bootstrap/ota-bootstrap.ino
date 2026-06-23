// =============================================================
//  Aquarium-Rechner — OTA-Bootstrap (Mini-Loader)
// =============================================================
//  Zweck: EINMAL flashen → der ESP holt sich automatisch die NEUESTE
//  Firmware aus den GitHub-Releases und installiert sie. Danach läuft die
//  volle Firmware. Du musst also nie mehr die aktuelle .bin manuell flashen.
//
//  Ablauf:
//    1. WLAN verbinden (Daten aus NVS, sonst aus den Feldern unten)
//    2. Neuestes GitHub-Release abfragen, .bin-Asset finden
//    3. Herunterladen + flashen (OTA) + Neustart in die volle Firmware
//
//  Hinweis: Eine PocketBase-Anmeldung ist NICHT nötig — die Releases sind
//  öffentlich. Login/Rolle macht danach die volle Firmware wie gewohnt.
//
//  ---------------------------------------------------------------
//  ARDUINO-IDE-EINSTELLUNGEN (GENAU wie bei der vollen Firmware!):
//    Board:            ESP32S3 Dev Module
//    Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
//    Flash Size:       16MB
//    PSRAM:            OPI PSRAM
//  (Nur mit diesem Partitionsschema funktioniert OTA — zwei App-Slots.)
//
//  BENÖTIGTE BIBLIOTHEK: ArduinoJson (v7) — sonst nur ESP32-Core.
//
//  FRISCHER ESP: trage unten dein WLAN ein. Bereits eingerichteter ESP:
//  WLAN-Daten liegen schon im NVS → einfach flashen, nichts ändern.
// =============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ---------- WLAN-Fallback (nur nötig, wenn NVS leer = fabrikneuer ESP) ----------
const char* WIFI_SSID_FALLBACK = "DEIN_WLAN";
const char* WIFI_PASS_FALLBACK = "DEIN_WLAN_PASSWORT";

// ---------- GitHub-Repo mit den Releases ----------
const char* GITHUB_OWNER  = "markus629";
const char* GITHUB_REPO   = "aquarium-rechner";
const char* ASSET_FILTER  = ".bin";   // Asset-Name muss das enthalten

// NVS-Namespace + Keys EXAKT wie in der vollen Firmware (config.h),
// damit ein bereits eingerichteter ESP seine WLAN-Daten wiederfindet.
const char* NVS_NS        = "aqrechner";
const char* NVS_WIFI_SSID = "wifi_ssid";
const char* NVS_WIFI_PASS = "wifi_pass";

// ---------- WLAN-Daten holen: erst NVS, sonst Fallback ----------
void loadWifi(String &ssid, String &pass) {
  Preferences prefs;
  prefs.begin(NVS_NS, true);            // read-only
  ssid = prefs.getString(NVS_WIFI_SSID, "");
  pass = prefs.getString(NVS_WIFI_PASS, "");
  prefs.end();
  if (ssid.length() == 0) {             // NVS leer → Fallback aus dem Sketch
    ssid = WIFI_SSID_FALLBACK;
    pass = WIFI_PASS_FALLBACK;
    Serial.println("[Bootstrap] WLAN aus Sketch-Fallback");
  } else {
    Serial.println("[Bootstrap] WLAN aus NVS (bereits eingerichtet)");
  }
}

bool connectWifi() {
  String ssid, pass;
  loadWifi(ssid, pass);
  Serial.printf("[Bootstrap] verbinde mit \"%s\" ...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[Bootstrap] WLAN fehlgeschlagen"); return false; }
  Serial.printf("[Bootstrap] verbunden, IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// ---------- Neueste Release-.bin-URL von GitHub holen ----------
String fetchLatestBinUrl(String &tagOut) {
  WiFiClientSecure client;
  client.setInsecure();                 // GitHub-Cert nicht pinnen (wie volle Firmware)
  HTTPClient http;
  String api = String("https://api.github.com/repos/") + GITHUB_OWNER + "/" + GITHUB_REPO + "/releases/latest";
  http.useHTTP10(true);                 // wichtig fürs Stream-Parsing
  http.addHeader("User-Agent", "aquarium-ota-bootstrap");
  http.addHeader("Accept", "application/vnd.github+json");
  if (!http.begin(client, api)) { Serial.println("[Bootstrap] http.begin fehlgeschlagen"); return ""; }
  int code = http.GET();
  if (code != 200) { Serial.printf("[Bootstrap] GitHub-API HTTP %d\n", code); http.end(); return ""; }

  // Nur tag_name + assets[*].{name,browser_download_url} parsen
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[Bootstrap] JSON-Fehler: %s\n", err.c_str()); return ""; }

  tagOut = String((const char*)(doc["tag_name"] | ""));
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    const char* name = asset["name"] | "";
    const char* url  = asset["browser_download_url"] | "";
    if (name && strstr(name, ASSET_FILTER) != nullptr) return String(url);
  }
  Serial.println("[Bootstrap] kein passendes .bin-Asset im Release");
  return "";
}

// ---------- Eigentliches OTA-Update ----------
void doOTA(const String &url) {
  Serial.printf("[Bootstrap] lade & flashe: %s\n", url.c_str());
  WiFiClientSecure client;
  client.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub leitet auf CDN um
  httpUpdate.rebootOnUpdate(true);
  Serial.println("[Bootstrap] Update läuft — NICHT ausschalten ...");
  t_httpUpdate_return ret = httpUpdate.update(client, url);
  // Bei Erfolg startet der ESP automatisch neu in die neue Firmware.
  if (ret == HTTP_UPDATE_FAILED)
    Serial.printf("[Bootstrap] FEHLER %d: %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
  else if (ret == HTTP_UPDATE_NO_UPDATES)
    Serial.println("[Bootstrap] keine Updates");
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== Aquarium-Rechner OTA-Bootstrap ===");

  if (!connectWifi()) { delay(5000); ESP.restart(); }   // kein WLAN → neu versuchen

  String tag;
  String binUrl = fetchLatestBinUrl(tag);
  if (binUrl.length() == 0) { Serial.println("[Bootstrap] kein Release gefunden — neuer Versuch in 30 s"); delay(30000); ESP.restart(); }

  Serial.printf("[Bootstrap] neueste Version: %s\n", tag.c_str());
  doOTA(binUrl);

  // Hierher kommen wir nur, wenn das Update NICHT durchlief → erneut probieren
  Serial.println("[Bootstrap] Update nicht erfolgreich — neuer Versuch in 30 s");
  delay(30000);
  ESP.restart();
}

void loop() { /* nichts — alles passiert einmalig in setup() */ }
