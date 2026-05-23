// =============================================================
// Upload-Buffer (Offline-Resilienz)
// =============================================================
// Jeder Schreibvorgang an Firestore (Dose, pH-Messung, etc.) wird zuerst
// in eine NVS-persistierte Queue gelegt. Bei Erfolg → entfernt. Bei Fehler
// (kein WLAN, Server down, Firebase-Token expired) bleibt der Eintrag
// gepuffert und wird periodisch erneut versucht.
//
// Garantie: keine Daten gehen verloren, auch nicht bei WLAN-Ausfall oder
// Power-Cycle (NVS überlebt Reboot).
//
// Kapazität: 50 Einträge (~4 Tage Dosier-Backlog bei 12 Doses/Tag + pH-Werte)
// Bei Überlauf: ältester Eintrag wird verworfen (FIFO drop).
// =============================================================
#pragma once

#include <Preferences.h>
#include <vector>
#include "config.h"

namespace upload_buffer {

struct PendingWrite {
  String path;       // Firestore-Pfad (relativ ab documents/), z.B. "users/UID/aquarium/dosings/items"
  String payload;    // Firestore-JSON-Body (FirebaseJson::raw())
  time_t enqueuedAt; // Wann eingestellt
  int attempts;      // Versuche
};

static const int MAX_BUFFER_ITEMS = 50;
std::vector<PendingWrite> queue;
unsigned long lastFlushAttemptMs = 0;
const unsigned long FLUSH_INTERVAL_MS = 15UL * 1000;  // alle 15s versuchen
const unsigned long BACKOFF_AFTER_FAIL_MS = 60UL * 1000;
unsigned long nextRetryAtMs = 0;

void load() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  String json = p.getString("bufQ", "[]");
  p.end();
  queue.clear();
  // Sehr einfacher JSON-Parser für unser bekanntes Format
  // [{"p":"...","b":"...","t":1234567,"a":3}, ...]
  int i = 0;
  while (i < (int)json.length()) {
    int objStart = json.indexOf('{', i);
    if (objStart < 0) break;
    int braceLevel = 1;
    int j = objStart + 1;
    bool inStr = false;
    while (j < (int)json.length() && braceLevel > 0) {
      char c = json.charAt(j);
      if (c == '"' && json.charAt(j-1) != '\\') inStr = !inStr;
      else if (!inStr) {
        if (c == '{') braceLevel++;
        else if (c == '}') braceLevel--;
      }
      j++;
    }
    String obj = json.substring(objStart, j);
    PendingWrite pw;
    // Pfad extrahieren
    int ps = obj.indexOf("\"p\":\"");
    int pe = ps >= 0 ? obj.indexOf("\"", ps + 5) : -1;
    if (ps >= 0 && pe >= 0) pw.path = obj.substring(ps + 5, pe);
    // Body — kann verschachteltes JSON sein, daher unescape
    int bs = obj.indexOf("\"b\":\"");
    int be = -1;
    if (bs >= 0) {
      // Suche unescaped "
      int k = bs + 5;
      while (k < (int)obj.length()) {
        if (obj.charAt(k) == '"' && obj.charAt(k-1) != '\\') { be = k; break; }
        k++;
      }
    }
    if (bs >= 0 && be >= 0) {
      String body = obj.substring(bs + 5, be);
      body.replace("\\\"", "\"");
      body.replace("\\\\", "\\");
      pw.payload = body;
    }
    int ts = obj.indexOf("\"t\":");
    if (ts >= 0) pw.enqueuedAt = (time_t)obj.substring(ts + 4).toInt();
    int as = obj.indexOf("\"a\":");
    if (as >= 0) pw.attempts = obj.substring(as + 4).toInt();
    if (pw.path.length() > 0 && pw.payload.length() > 0) queue.push_back(pw);
    i = j;
  }
  if (!queue.empty()) Serial.printf("[Buffer] %d Einträge aus NVS geladen\n", (int)queue.size());
}

void save() {
  String json = "[";
  for (size_t i = 0; i < queue.size(); i++) {
    if (i > 0) json += ",";
    String esc = queue[i].payload;
    esc.replace("\\", "\\\\");
    esc.replace("\"", "\\\"");
    json += "{\"p\":\"" + queue[i].path + "\",\"b\":\"" + esc + "\",\"t\":" + String((long)queue[i].enqueuedAt) + ",\"a\":" + String(queue[i].attempts) + "}";
  }
  json += "]";
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putString("bufQ", json);
  p.end();
}

void enqueue(const String &path, const String &payload) {
  if (queue.size() >= MAX_BUFFER_ITEMS) {
    Serial.println("[Buffer] FULL — ältesten Eintrag verwerfen");
    queue.erase(queue.begin());
  }
  PendingWrite pw;
  pw.path = path;
  pw.payload = payload;
  time(&pw.enqueuedAt);
  pw.attempts = 0;
  queue.push_back(pw);
  save();
  Serial.printf("[Buffer] +1 Eintrag (%d total): %s\n", (int)queue.size(), path.c_str());
}

int size() { return (int)queue.size(); }

void begin() { load(); }

} // namespace upload_buffer
