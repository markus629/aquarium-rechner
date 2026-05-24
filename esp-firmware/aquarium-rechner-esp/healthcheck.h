// =============================================================
// Healthcheck-Ping (Dead-Man's-Switch)
// =============================================================
// Pingt eine externe URL (z.B. https://hc-ping.com/<uuid> für
// healthchecks.io). Wenn der Service eine bestimmte Zeit lang
// keinen Ping mehr bekommt, schickt er Email/Telegram/Discord etc.
//
// → User wird benachrichtigt wenn ESP offline, hängt, oder kein Strom hat.
// =============================================================
#pragma once

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "settings_cache.h"

namespace healthcheck {

unsigned long lastPingMs = 0;
const unsigned long MIN_PING_INTERVAL_MS = 25UL * 1000UL;  // max 1 Ping/25s, schont das Free-Tier-Limit

// Sendet einen kurzen GET an die konfigurierte URL.
// Nicht-blockierend gestaltet: kurzer Timeout, Fehler werden ignoriert
// (nächster Heartbeat versucht's wieder, wichtiger ist dass der Service
// merkt wenn wir DAUERHAFT nichts schicken).
void ping() {
  if (settings_cache::healthcheckUrl.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (lastPingMs != 0 && now - lastPingMs < MIN_PING_INTERVAL_MS) return;
  lastPingMs = now;

  const String &url = settings_cache::healthcheckUrl;
  HTTPClient http;
  bool ok = false;

  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure();  // healthchecks.io hat valides Cert, wir sparen uns das Pinning
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (http.begin(client, url)) {
      int code = http.GET();
      ok = (code >= 200 && code < 300);
      http.end();
    }
  } else if (url.startsWith("http://")) {
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (http.begin(url)) {
      int code = http.GET();
      ok = (code >= 200 && code < 300);
      http.end();
    }
  }
  // Bewusst kein Serial-Spam — würde alle 30s den Log fluten.
  // Bei Fehler ignoriert: healthchecks.io meldet eh wenn nichts mehr kommt.
  (void)ok;
}

} // namespace healthcheck
