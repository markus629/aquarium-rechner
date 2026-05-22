// =============================================================
//   Aquarium-Rechner ESP-Firmware
//   ===============================
//   Board:      ESP32-S3 N16R8 (16 MB Flash, 8 MB PSRAM)
//   IDE:        Arduino 2.x mit "ESP32 by Espressif Systems" Boards
//   Libraries:  Firebase ESP Client (Mobizt), FastAccelStepper, ArduinoJson v7
//
//   Session A (jetzt):
//     - WiFi-Setup-Portal (Captive Portal)
//     - Firebase Auth (E-Mail + Passwort)
//     - Heartbeat alle 30 s
//     - Command-Polling-Struktur (adaptive Geschwindigkeit)
//     - pH-Sensor mit Mittelwert
//     - Stepper-Init (4 Pumpen)
//     - OTA-Update-Check (alle 6 h)
//     - 3× Power-Cycle = Setup-Reset
//
//   Session B (folgt):
//     - Dosier-Plan ausführen
//     - Live-Kalibrierung
//     - Manuelle Commands abarbeiten
//     - NTP + DS3231 RTC-Sync
//     - LittleFS Plan-Cache (Offline-Fallback)
// =============================================================

#include <Arduino.h>
#include <WiFi.h>
#include "time.h"
#include "config.h"
#include "wifi_setup.h"
#include "firebase_sync.h"
#include "ph_sensor.h"
#include "pumps.h"
#include "plan_executor.h"
#include "ota_update.h"

unsigned long lastHeartbeatMs = 0;
unsigned long bootMs = 0;
unsigned long lastPhReportMs = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("==========================================");
  Serial.printf(" %s v%s booting …\n", FW_NAME, FW_VERSION);
  Serial.println("==========================================");
  bootMs = millis();

  // 3× Power-Cycle erkennen → Setup-Reset
  setup_portal::checkBootCount();

  // Hardware-Init zuerst (Pumpen sicher in disabled state)
  pumps::begin();
  ph_sensor::begin();

  // Keine gespeicherte Config? → Setup-Portal
  if (!setup_portal::hasStoredConfig()) {
    Serial.println("[Boot] Keine Config — Setup-Portal startet");
    setup_portal::runSetupPortal();
    // runSetupPortal blockiert bis Reboot
    return;
  }

  // Config laden
  String ssid, wifiPass, fbEmail, fbPass;
  setup_portal::getConfig(ssid, wifiPass, fbEmail, fbPass);

  // WLAN verbinden
  if (!setup_portal::connectWifi(ssid, wifiPass)) {
    Serial.println("[Boot] WLAN fehlgeschlagen — Setup-Portal");
    setup_portal::runSetupPortal();
    return;
  }

  // NTP für Zeitstempel (Berlin-Zeitzone)
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  // Boot-Count zurücksetzen sobald wir stabil 4 s laufen
  // → erfolgt im Loop nach 4 s

  // Firebase einloggen
  if (!firebase_sync::begin(fbEmail, fbPass)) {
    Serial.println("[Boot] Firebase-Login fehlgeschlagen — bleibe trotzdem online, retry später");
    // Wir machen weiter — Heartbeat schlägt fehl, aber wenigstens nicht im Setup gefangen
  }

  Serial.println("[Boot] Setup fertig — Loop startet");
}

void loop() {
  // Boot-Count reset nach 4 s stabiler Uptime
  static bool bootCountReset = false;
  if (!bootCountReset && millis() - bootMs > 4000) {
    setup_portal::resetBootCount();
    bootCountReset = true;
  }

  // pH-Sampling (alle 100 ms)
  ph_sensor::tick();

  // Pumpen-Check (haben sie fertig dosiert?)
  int finishedPump = pumps::checkAndDisable();
  if (finishedPump >= 0) {
    Serial.printf("[Loop] Pumpe %d (%s) fertig\n", finishedPump, pumps::PUMP_NAMES[finishedPump]);
    // TODO Session B: addDosing-Bestätigung schreiben
  }

  // Plan + Commands (adaptives Polling intern)
  plan_executor::tick();

  // Heartbeat
  unsigned long now = millis();
  if (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    float ph = ph_sensor::getPH();
    long uptime = (now - bootMs) / 1000;
    if (firebase_sync::sendHeartbeat(ph, ph_sensor::getSampleCount(), uptime, 0)) {
      Serial.printf("[HB] OK  uptime=%lds  pH=%.2f  RSSI=%d\n", uptime, ph, WiFi.RSSI());
    } else {
      Serial.println("[HB] FAIL");
    }
  }

  // pH-Messung alle 5 Min separat in measurements/items schreiben
  if (ph_sensor::isCalibrated() && now - lastPhReportMs > PH_REPORT_INTERVAL_MS) {
    lastPhReportMs = now;
    float ph = ph_sensor::getPH();
    if (!isnan(ph)) firebase_sync::addPhMeasurement(ph);
  }

  // OTA-Check alle 6 h
  ota_update::tick();

  // WLAN-Watchdog: wenn wir die Verbindung verlieren, retry
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] verloren — versuche Reconnect");
    WiFi.reconnect();
    delay(5000);
  }

  delay(50);
}
