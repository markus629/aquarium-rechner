// =============================================================
// WiFi + Backend-Setup-Portal
// =============================================================
// Erstinstallation: ESP startet Access-Point + Captive-Portal-Webserver.
// Nutzer trägt WLAN + Account-Credentials in HTML-Form ein.
// Nach Speichern: Reboot → ESP verbindet sich autonomously.
// =============================================================
#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

namespace setup_portal {

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
bool saved = false;

const char SETUP_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="de"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Aquarium-Rechner Setup</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: linear-gradient(135deg, #3498db, #2980b9); min-height: 100vh;
  display: flex; align-items: center; justify-content: center; padding: 1rem; color: #2c3e50; }
.card { background: #fff; border-radius: 16px; padding: 1.8rem; max-width: 420px; width: 100%;
  box-shadow: 0 10px 40px rgba(0,0,0,0.2); }
h1 { color: #2980b9; font-size: 1.4rem; margin-bottom: 0.3rem; }
p.sub { color: #7a8896; font-size: 0.88rem; margin-bottom: 1.2rem; }
h2 { font-size: 0.78rem; text-transform: uppercase; letter-spacing: 0.08em; color: #5a6776;
  margin: 1.2rem 0 0.5rem; padding-bottom: 0.3rem; border-bottom: 1px solid #e1e8ed; }
label { display: block; font-size: 0.78rem; font-weight: 600; color: #5a6776; margin: 0.7rem 0 0.3rem; }
input, select { width: 100%; padding: 0.6rem 0.7rem; border: 2px solid #e1e8ed; border-radius: 8px;
  font-size: 0.95rem; font-family: inherit; }
input:focus, select:focus { outline: none; border-color: #3498db; }
button { width: 100%; padding: 0.8rem; background: linear-gradient(135deg, #3498db, #2980b9);
  color: #fff; border: none; border-radius: 8px; font-size: 1rem; font-weight: 700;
  cursor: pointer; margin-top: 1.5rem; font-family: inherit; }
button:hover { box-shadow: 0 4px 12px rgba(52,152,219,0.4); }
.note { font-size: 0.78rem; color: #7a8896; margin-top: 0.8rem; line-height: 1.5; }
</style></head><body>
<form class="card" method="POST" action="/save">
  <h1>💧 Aquarium-Rechner</h1>
  <p class="sub">Erstkonfiguration deines ESP-Geräts</p>

  <h2>WLAN-Verbindung</h2>
  <label>Netzwerk-Name (SSID)</label>
  <select name="ssid" required>%SSID_OPTIONS%</select>
  <label>WLAN-Passwort</label>
  <input type="password" name="wifi_pass" placeholder="Lass leer wenn offen">

  <h2>Aquarium-Rechner-Konto</h2>
  <p class="note" style="margin:0 0 0.5rem">Deine Zugangsdaten (E-Mail + Passwort) für den Aquarium-Rechner — dieselben wie beim Web-Login. Der ESP nutzt sie, um deine Daten zu lesen/schreiben.</p>
  <div style="background:#e8f4fd;border-left:3px solid #3498db;padding:0.6rem 0.8rem;border-radius:6px;margin:0.5rem 0;font-size:0.78rem;line-height:1.5">
    Noch kein Konto? Im Web einmal registrieren (E-Mail + Passwort), dann hier dieselben Daten eintragen.
  </div>
  <label>E-Mail</label>
  <input type="email" name="fb_email" required autocomplete="email">
  <label>Passwort</label>
  <input type="password" name="fb_password" required autocomplete="current-password">

  <button type="submit">Speichern & Verbinden</button>
  <p class="note">Falls du das WLAN oder den Account später ändern willst:
    ESP <strong>3× kurz hintereinander stromlos machen</strong> &mdash; dann zurück in diesen Setup-Modus.</p>
</form>
</body></html>
)HTML";

String buildSsidOptions() {
  String out = "<option value=\"\">— scannen … —</option>";
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    out += "<option value=\"" + ssid + "\">" + ssid + " (" + WiFi.RSSI(i) + " dBm)</option>";
  }
  return out;
}

void handleRoot() {
  String html(SETUP_HTML);
  html.replace("%SSID_OPTIONS%", buildSsidOptions());
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String wifiPass = server.arg("wifi_pass");
  String email = server.arg("fb_email");
  String fbPass = server.arg("fb_password");

  if (ssid.length() == 0 || email.length() == 0 || fbPass.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "Bitte alle Pflichtfelder ausfüllen.");
    return;
  }

  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_KEY_WIFI_SSID, ssid);
  prefs.putString(NVS_KEY_WIFI_PASS, wifiPass);
  prefs.putString(NVS_KEY_FB_EMAIL, email);
  prefs.putString(NVS_KEY_FB_PASSWORD, fbPass);
  prefs.end();

  server.send(200, "text/html; charset=utf-8",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Gespeichert</title>"
    "<style>body{font-family:-apple-system,sans-serif;background:#27ae60;color:#fff;display:flex;"
    "align-items:center;justify-content:center;min-height:100vh;margin:0;padding:1rem;text-align:center}"
    "h1{font-size:2rem}p{margin-top:1rem;opacity:0.9}</style></head>"
    "<body><div><h1>✅ Gespeichert</h1><p>ESP startet neu &hellip; in ein paar Sekunden online.</p></div></body></html>");
  saved = true;
}

void handleNotFound() {
  // Captive-Portal-Redirect: alle Anfragen auf unsere Setup-Seite umleiten
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

bool hasStoredConfig() {
  prefs.begin(NVS_NAMESPACE, true);
  bool ok = prefs.isKey(NVS_KEY_WIFI_SSID) && prefs.isKey(NVS_KEY_FB_EMAIL);
  prefs.end();
  return ok;
}

void clearConfig() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.remove(NVS_KEY_WIFI_SSID);
  prefs.remove(NVS_KEY_WIFI_PASS);
  prefs.remove(NVS_KEY_FB_EMAIL);
  prefs.remove(NVS_KEY_FB_PASSWORD);
  prefs.end();
}

void getConfig(String &ssid, String &wifiPass, String &fbEmail, String &fbPass) {
  prefs.begin(NVS_NAMESPACE, true);
  ssid = prefs.getString(NVS_KEY_WIFI_SSID, "");
  wifiPass = prefs.getString(NVS_KEY_WIFI_PASS, "");
  fbEmail = prefs.getString(NVS_KEY_FB_EMAIL, "");
  fbPass = prefs.getString(NVS_KEY_FB_PASSWORD, "");
  prefs.end();
}

// ---------- Setup-Modus starten ----------
// Blockiert (mit yield) bis Konfiguration eingegeben wurde ODER Timeout.
// Nach Speichern wird ESP intern resettet.
void runSetupPortal() {
  Serial.println("[Setup] Starte Access-Point …");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("[Setup] AP-IP: "); Serial.println(apIP);

  dnsServer.start(53, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  unsigned long start = millis();
  while (!saved && (millis() - start) < SETUP_AP_TIMEOUT_MS) {
    dnsServer.processNextRequest();
    server.handleClient();
    yield();
  }

  if (saved) {
    delay(2000);
    Serial.println("[Setup] Konfiguration gespeichert — Reboot …");
    ESP.restart();
  } else {
    Serial.println("[Setup] Timeout — Reboot in Setup-Modus");
    ESP.restart();
  }
}

// ---------- WLAN verbinden ----------
bool connectWifi(const String &ssid, const String &pass, unsigned long timeoutMs = 30000) {
  Serial.printf("[WiFi] Verbinde mit \"%s\" …\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(FW_NAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] verbunden — IP %s, RSSI %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("[WiFi] FEHLER: keine Verbindung");
  return false;
}

// ---------- Boot-Detection: 3× Power-Cycle = Setup-Reset ----------
// Beim Boot wird boot_count erhöht. Nach 4 s wird auf 0 zurückgesetzt.
// Wenn jemand 3× hintereinander vorher Strom kappt: count = 3 → Setup-Reset.
void checkBootCount() {
  prefs.begin(NVS_NAMESPACE, false);
  int count = prefs.getInt(NVS_KEY_BOOT_COUNT, 0) + 1;
  prefs.putInt(NVS_KEY_BOOT_COUNT, count);
  prefs.end();
  Serial.printf("[Boot] count=%d\n", count);

  if (count >= 3) {
    Serial.println("[Boot] 3× schneller Power-Cycle erkannt — Reset auf Setup-Modus");
    clearConfig();
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt(NVS_KEY_BOOT_COUNT, 0);
    prefs.end();
  }
}

void resetBootCount() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(NVS_KEY_BOOT_COUNT, 0);
  prefs.end();
}

} // namespace setup_portal
