// =============================================================
// Backend-Anbindung — Auth + Reads/Writes (PocketBase REST)
// =============================================================
// PocketBase-Backend über HTTPS-REST mit ArduinoJson (v7).
//
// Auth-Flow:
//   - Login mit E-Mail + Passwort (vom Setup-Portal) →
//     POST /api/collections/users/auth-with-password → JWT-Token
//   - Token wird mitgesendet; bei HTTP 401 automatisch neu eingeloggt.
//
// Datenmodell (Collections):
//   aqua_docs       key=info|settings|plan-current|ph-calibration|pump-N  (1 Doc/User, Feld "data")
//   aqua_measurements  (type,value,timestamp,source)   ← pH-Werte (write)
//   aqua_dosings       (pump,ml,...,timestamp,source)  ← Dose-Bestätigungen (write)
//   aqua_command       (cmdId,action,status,...)       ← Command (read+update)
//
// HINWEIS: Namespace heißt weiter "firebase_sync", damit die Aufrufer
// (plan_executor.h, settings_cache.h, .ino) unverändert bleiben.
// =============================================================
#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include "upload_buffer.h"

#if PB_TLS_INSECURE == 0
// ISRG Root X1 (Let's Encrypt) — hier den vollständigen PEM-Block einsetzen,
// wenn Zertifikatsprüfung gewünscht ist (PB_TLS_INSECURE 0 in config.h).
static const char *PB_ROOT_CA = R"CERT(
-----BEGIN CERTIFICATE-----
... ISRG Root X1 PEM hier einfuegen ...
-----END CERTIFICATE-----
)CERT";
#endif

namespace firebase_sync {

static WiFiClientSecure pbClient;
static HTTPClient pbHttp;

String authToken;
String currentUid;
String credEmail, credPassword;
bool ready = false;

// PocketBase identifiziert Records per id (nicht per Pfad) → IDs cachen.
String idCommand;
String idAquaDoc[6];  // 0=info 1=settings 2=plan-current 3=ph-calibration 4=livePh 5=_livereq
inline String& cacheIdFor(const String &key) {
  if (key == "info") return idAquaDoc[0];
  if (key == "settings") return idAquaDoc[1];
  if (key == "plan-current") return idAquaDoc[2];
  if (key == "ph-calibration") return idAquaDoc[3];
  if (key == "livePh") return idAquaDoc[4];
  if (key == "_livereq") return idAquaDoc[5];
  static String dummy; dummy = ""; return dummy;  // pump-N etc.: nicht gecacht
}

bool authenticate();  // fwd

// ---------- TLS konfigurieren ----------
static void configureTLS() {
#if PB_TLS_INSECURE
  pbClient.setInsecure();          // keine Zertifikatsprüfung (siehe config.h)
#else
  pbClient.setCACert(PB_ROOT_CA);
#endif
}

// ---------- URL-Encode (für Filter-Strings) ----------
static String urlEncode(const String &s) {
  String out; char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
    else { sprintf(buf, "%%%02X", (uint8_t)c); out += buf; }
  }
  return out;
}

// ---------- Low-level Request ----------
// method: GET/POST/PATCH/DELETE. path: ab "/api/...". body: JSON oder "".
// respOut bekommt den Response-Body. Returns HTTP-Status (<0 = Verbindungsfehler).
// Bei 401 wird einmal re-authentifiziert und der Request wiederholt.
static int request(const char *method, const String &path, const String &body,
                   String *respOut, bool allowReauth = true) {
  configureTLS();
  if (!pbHttp.begin(pbClient, String(PB_URL) + path)) return -1;
  pbHttp.setReuse(true);
  pbHttp.setTimeout(15000);
  if (authToken.length()) pbHttp.addHeader("Authorization", authToken);
  if (body.length())      pbHttp.addHeader("Content-Type", "application/json");

  int code;
  if (strcmp(method, "GET") == 0) code = pbHttp.GET();
  else code = pbHttp.sendRequest(method, (uint8_t *)body.c_str(), body.length());

  if (respOut) *respOut = pbHttp.getString();
  pbHttp.end();

  if (code == 401 && allowReauth) {
    authToken = "";
    if (authenticate()) return request(method, path, body, respOut, false);
  }
  return code;
}

// ---------- Auth ----------
bool authenticate() {
  JsonDocument body;
  body["identity"] = credEmail;
  body["password"] = credPassword;
  String bodyStr; serializeJson(body, bodyStr);

  String resp;
  authToken = "";  // ohne Token anfragen
  int code = request("POST", "/api/collections/users/auth-with-password", bodyStr, &resp, false);
  if (code != 200) { Serial.printf("[PB] Auth FAIL HTTP %d: %s\n", code, resp.c_str()); return false; }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  authToken  = doc["token"].as<String>();
  currentUid = doc["record"]["id"].as<String>();
  return authToken.length() > 0 && currentUid.length() > 0;
}

bool begin(const String &email, const String &password) {
  credEmail = email; credPassword = password;
  Serial.println("[PB] Authentifiziere …");
  ready = authenticate();
  if (ready) Serial.printf("[PB] eingeloggt als uid=%s\n", currentUid.c_str());
  else       Serial.println("[PB] FEHLER: Auth fehlgeschlagen");
  return ready;
}

bool isReady() { return ready && WiFi.status() == WL_CONNECTED; }
const String& uid() { return currentUid; }

// ---------- aqua_docs Helfer ----------
// Sucht die Record-ID des aqua_docs <key>. "" wenn nicht vorhanden.
static String findAquaDocId(const String &key) {
  if (!isReady()) return "";
  String filter = "user='" + currentUid + "' && key='" + key + "'";
  String path = "/api/collections/aqua_docs/records?perPage=1&filter=" + urlEncode(filter);
  String resp;
  if (request("GET", path, "", &resp) != 200) return "";
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return "";
  JsonArray items = doc["items"].as<JsonArray>();
  if (items.size() == 0) return "";
  return items[0]["id"].as<String>();
}

// Lädt das "data"-Objekt des aqua_docs <key> nach out. Returns true wenn vorhanden.
static bool fetchAquaDoc(const String &key, JsonDocument &out) {
  if (!isReady()) return false;
  String filter = "user='" + currentUid + "' && key='" + key + "'";
  String path = "/api/collections/aqua_docs/records?perPage=1&filter=" + urlEncode(filter);
  String resp;
  if (request("GET", path, "", &resp) != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  JsonArray items = doc["items"].as<JsonArray>();
  if (items.size() == 0) return false;
  String &cid = cacheIdFor(key);
  if (cid.length() == 0) cid = items[0]["id"].as<String>();
  out.set(items[0]["data"]);   // tiefe Kopie des data-Objekts
  return true;
}

// Schreibt das data-Objekt unter <key> (upsert, ersetzt data komplett).
static bool upsertAquaDoc(const String &key, JsonVariantConst data) {
  if (!isReady()) return false;
  String &cid = cacheIdFor(key);
  if (cid.length() == 0) cid = findAquaDocId(key);

  String resp; int code = 0;
  if (cid.length()) {
    JsonDocument body; body["data"] = data;
    String b; serializeJson(body, b);
    code = request("PATCH", "/api/collections/aqua_docs/records/" + cid, b, &resp);
    if (code == 404) cid = "";  // Record verschwunden → neu anlegen
  }
  if (cid.length() == 0) {
    JsonDocument body;
    body["user"] = currentUid; body["key"] = key; body["data"] = data;
    String b; serializeJson(body, b);
    code = request("POST", "/api/collections/aqua_docs/records", b, &resp);
    if (code == 200) {
      JsonDocument d;
      if (!deserializeJson(d, resp)) cid = d["id"].as<String>();
    }
  }
  return code == 200;
}

// ---------- Heartbeat schreiben (aqua_docs key="info") ----------
struct HeartbeatStats {
  unsigned long dosesTotal;
  unsigned long dosesFailedTotal;
  int dosesOk24h;
  int dosesFail24h;
  bool pumpsCalibrated[4];  // mlPerStep > 0 ?
  int bufferQueueSize;
};

bool sendHeartbeat(float phValue, int phSamples, long uptimeSec, const HeartbeatStats &stats) {
  if (!isReady()) return false;
  JsonDocument data;
  data["online"]   = true;
  data["firmware"] = FW_VERSION;
  time_t now; time(&now);
  if (now > 1700000000) data["lastSeen"] = (long)now;   // Unix-Sekunden (Web rechnet *1000)
  data["freeHeap"]  = (int)ESP.getFreeHeap();
  data["rssi"]      = (int)WiFi.RSSI();
  data["ip"]        = WiFi.localIP().toString();
  data["uptimeSec"] = (int)uptimeSec;
  if (!isnan(phValue)) {
    data["lastPh"]        = phValue;
    data["lastPhSamples"] = phSamples;
  }
  data["dosesTotal"]       = (int)stats.dosesTotal;
  data["dosesFailedTotal"] = (int)stats.dosesFailedTotal;
  data["dosesOk24h"]       = stats.dosesOk24h;
  data["dosesFail24h"]     = stats.dosesFail24h;
  data["bufferQueueSize"]  = stats.bufferQueueSize;
  JsonArray pc = data["pumpsCalibrated"].to<JsonArray>();
  for (int i = 0; i < 4; i++) pc.add(stats.pumpsCalibrated[i]);

  return upsertAquaDoc("info", data);
}

// ---------- Settings lesen ----------
bool fetchSettings(JsonDocument &out) { return fetchAquaDoc("settings", out); }

// ---------- Plan lesen ----------
bool fetchPlan(JsonDocument &out) { return fetchAquaDoc("plan-current", out); }

// ---------- Messung / Dosis puffern ----------
void addPhMeasurement(float phValue) { upload_buffer::enqueuePh(phValue); }
void addDosing(int pumpIdx, float ml, const char *dosageType, bool isAutomatic, float factor, bool success) {
  (void)factor;  // aktuell immer 1.0 → beim Senden rekonstruiert
  upload_buffer::enqueueDose(pumpIdx, ml, dosageType, isAutomatic, success);
}

// ---------- Container-Level dekrementieren ----------
void decrementContainerLevel(int pumpIdx, float ml) {
  if (!isReady() || pumpIdx < 0 || pumpIdx > 3 || ml <= 0) return;
  JsonDocument data;
  if (!fetchAquaDoc("settings", data)) return;
  JsonArray levels = data["containerLevel"].as<JsonArray>();
  if (levels.isNull() || levels.size() < 4) {
    // Feld fehlt → mit Defaults anlegen
    levels = data["containerLevel"].to<JsonArray>();
    for (int i = 0; i < 4; i++) levels.add(5000.0);
  }
  float cur = levels[pumpIdx].as<float>();
  cur -= ml; if (cur < 0) cur = 0;
  levels[pumpIdx] = cur;
  upsertAquaDoc("settings", data);  // ganzes data zurückschreiben (andere Felder bleiben)
}

// ---------- pH-Kalibrierung puffern ----------
void writePhCalibration(float voltage_pH4, float voltage_pH7, bool isCalibrated) {
  upload_buffer::enqueuePhCal(voltage_pH4, voltage_pH7, isCalibrated);
}

// ---------- pH-Kalibrierung lesen ----------
bool fetchPhCalibration(float &v4_out, float &v7_out, bool &calibrated_out) {
  if (!isReady()) return false;
  JsonDocument data;
  if (!fetchAquaDoc("ph-calibration", data)) return false;
  if (data["voltage_pH4"].is<float>()) v4_out = data["voltage_pH4"].as<float>();
  if (data["voltage_pH7"].is<float>()) v7_out = data["voltage_pH7"].as<float>();
  calibrated_out = !isnan(v4_out) && !isnan(v7_out);
  return true;
}

// ---------- Live-pH (On-Demand) ----------
// Schaut gerade jemand im UI zu? UI setzt aqua_docs key="_livereq" data.until
// (Unix-Sekunden) und frischt es regelmäßig auf. true = aktiv.
bool livePhRequested() {
  if (!isReady()) return false;
  JsonDocument d;
  if (!fetchAquaDoc("_livereq", d)) return false;
  long until = d["until"] | 0L;
  time_t now; time(&now);
  return now > 1700000000 && until > (long)now;
}

// Aktuellen pH-Wert + Spannung live veröffentlichen — flüchtig (eigener Key,
// überschreibt sich, KEIN Graph-Punkt). UI abonniert aqua_docs key="livePh".
// Wichtig: auch während der Kalibrierung pushen (pH evtl. NaN) → die Spannung
// zeigt im UI live, wie sich der Sensor in den Pufferlösungen bewegt.
void publishLive(float ph, float voltage) {
  if (!isReady() || isnan(voltage)) return;
  JsonDocument d;
  if (!isnan(ph)) d["ph"] = ph;       // pH nur wenn kalibriert + plausibel
  d["voltage"] = voltage;
  d["ts"] = (long)time(nullptr);
  upsertAquaDoc("livePh", d);
}

// ---------- Buffer-Flush ----------
bool flushBuffer() {
  if (!isReady() || upload_buffer::queue.empty()) return true;
  unsigned long ms = millis();
  if (upload_buffer::nextRetryAtMs != 0 && ms < upload_buffer::nextRetryAtMs) return false;
  if (ms - upload_buffer::lastFlushAttemptMs < upload_buffer::FLUSH_INTERVAL_MS) return false;
  upload_buffer::lastFlushAttemptMs = ms;

  upload_buffer::PendingWrite &first = upload_buffer::queue.front();
  first.attempts++;
  bool ok = false;
  String resp;

  if (first.kind == upload_buffer::KIND_DOSE) {
    JsonDocument body;
    body["user"]        = currentUid;
    body["pump"]        = (int)first.pump;
    body["ml"]          = first.ml;
    body["dosageType"]  = upload_buffer::doseTypeToStr(first.doseType);
    body["isAutomatic"] = first.isAuto;
    body["factor"]      = 1.0;
    body["success"]     = first.success;
    body["source"]      = "esp";
    if (first.timestamp > 1700000000) body["timestamp"] = (long)first.timestamp;
    String b; serializeJson(body, b);
    ok = request("POST", "/api/collections/aqua_dosings/records", b, &resp) == 200;

  } else if (first.kind == upload_buffer::KIND_PH) {
    JsonDocument body;
    body["user"]   = currentUid;
    body["type"]   = "ph";
    body["value"]  = first.phValue;
    body["source"] = "esp";
    if (first.timestamp > 1700000000) body["timestamp"] = (long)first.timestamp;
    String b; serializeJson(body, b);
    ok = request("POST", "/api/collections/aqua_measurements/records", b, &resp) == 200;

  } else {  // KIND_PHCAL → aqua_docs key="ph-calibration" (Merge mit Bestand)
    JsonDocument data;
    fetchAquaDoc("ph-calibration", data);  // evtl. leer
    if (!isnan(first.v4)) data["voltage_pH4"] = first.v4;
    if (!isnan(first.v7)) data["voltage_pH7"] = first.v7;
    data["isCalibrated"] = first.calibrated;
    if (first.timestamp > 1700000000) data["lastCalibratedAt"] = (long)first.timestamp;
    ok = upsertAquaDoc("ph-calibration", data);
  }

  if (ok) {
    if (first.kind == upload_buffer::KIND_DOSE && first.success) {
      decrementContainerLevel(first.pump, first.ml);
    }
    Serial.printf("[Buffer] flushed (%d remaining)\n", (int)upload_buffer::queue.size() - 1);
    upload_buffer::queue.erase(upload_buffer::queue.begin());
    upload_buffer::save();
    upload_buffer::nextRetryAtMs = 0;
    upload_buffer::lastFlushAttemptMs = 0;  // großen Backlog schnell abarbeiten
    return true;
  } else {
    Serial.printf("[Buffer] flush FAIL (attempts=%d, %d in queue)\n",
                  first.attempts, (int)upload_buffer::queue.size());
    upload_buffer::nextRetryAtMs = ms + upload_buffer::BACKOFF_AFTER_FAIL_MS;
    upload_buffer::save();
    return false;
  }
}

// ---------- Pump-Konfig lesen (Kalibrierung) ----------
bool fetchPumpStepsPerML(int pumpIdx, float &outStepsPerML) {
  if (!isReady()) return false;
  JsonDocument data;
  if (!fetchAquaDoc("pump-" + String(pumpIdx), data)) return false;
  if (data["stepsPerML"].is<float>()) {
    outStepsPerML = data["stepsPerML"].as<float>();
    return true;
  }
  return false;
}

// ---------- Aktuelles Command lesen (aqua_command, 1 Record/User) ----------
// Füllt out mit dem Record (cmdId, action, status, pump, ml, steps, phValue).
// Returns true wenn ein Command-Record existiert.
bool fetchActiveCommand(JsonDocument &out) {
  if (!isReady()) return false;
  String filter = "user='" + currentUid + "'";
  String path = "/api/collections/aqua_command/records?perPage=1&filter=" + urlEncode(filter);
  String resp;
  if (request("GET", path, "", &resp) != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  JsonArray items = doc["items"].as<JsonArray>();
  if (items.size() == 0) return false;
  idCommand = items[0]["id"].as<String>();
  out.set(items[0]);
  return true;
}

// ---------- Command-Status aktualisieren ----------
bool updateCommandStatus(const String &status, JsonDocument *resultJson = nullptr) {
  if (!isReady()) return false;
  // idCommand wird von fetchActiveCommand() gesetzt (immer vor Status-Updates aufgerufen).
  if (idCommand.length() == 0) return false;
  JsonDocument body;
  body["status"] = status;
  if (resultJson) body["result"] = *resultJson;
  String b; serializeJson(body, b);
  String resp;
  bool ok = request("PATCH", "/api/collections/aqua_command/records/" + idCommand, b, &resp) == 200;
  if (!ok) Serial.printf("[Cmd] update FAIL: %s\n", resp.c_str());
  return ok;
}

} // namespace firebase_sync
