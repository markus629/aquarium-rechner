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
#include "rtc_sync.h"
#include "settings_cache.h"
#include "upload_buffer.h"
#include "plan_executor.h"
#include "ota_update.h"
#include "healthcheck.h"

unsigned long lastHeartbeatMs = 0;
unsigned long bootMs = 0;

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

  // Settings ZUERST aus NVS laden, damit pumps::begin() bereits mit den
  // korrekten stepsPerSec/accelPerSec2 Werten den Stepper initialisiert
  // (statt mit hardcoded Defaults — siehe Audit).
  settings_cache::loadFromNVS();

  // Hardware-Init (Pumpen sicher in disabled state)
  pumps::begin();
  ph_sensor::begin();
  rtc_sync::begin();   // DS3231 lesen → System-Zeit setzen (falls vorhanden + batteriegepuffert)
  upload_buffer::begin();   // lädt offline-gepufferte Doses/pH aus NVS
  plan_executor::begin();   // lädt Plan-Cache + pH-Kalib aus NVS

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

  // NTP für Zeitstempel (UTC — DS3231 hält ebenfalls UTC, Zeitzone via configTzTime
  // wirkt nur auf localtime_r-Ausgabe, time() bleibt UTC)
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

  // Plan + Commands (adaptives Polling intern, übernimmt auch Pump-Check + finishCommand)
  plan_executor::tick();

  // Heartbeat
  unsigned long now = millis();
  if (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    // pH nur alle 10 Heartbeats (= alle 5 min) mitschicken — die Tag/Nacht-Logik
    // läuft ohnehin lokal, das hier ist nur Live-Anzeige im Web-Dashboard.
    static uint8_t hbCount = 0;
    bool includePh = (hbCount++ % 10 == 0);
    float ph = includePh ? ph_sensor::getPH() : NAN;
    int phSamples = includePh ? ph_sensor::getSampleCount() : 0;
    long uptime = (now - bootMs) / 1000;
    firebase_sync::HeartbeatStats stats;
    stats.dosesTotal = plan_executor::dosesTotal;
    stats.dosesFailedTotal = plan_executor::dosesFailedTotal;
    plan_executor::countDosesLast24h(stats.dosesOk24h, stats.dosesFail24h);
    stats.bufferQueueSize = upload_buffer::size();
    for (int i = 0; i < 4; i++) {
      stats.pumpsCalibrated[i] = pumps::stepsPerML[i] > 0.0f;
    }
    // Dead-Man's-Switch: externe Healthcheck-URL pingen (z.B. healthchecks.io)
    // Wenn der ESP ausfällt, bleibt der Ping aus → User bekommt Email/Telegram
    healthcheck::ping();

    if (firebase_sync::sendHeartbeat(ph, phSamples, uptime, stats)) {
      if (includePh) {
        Serial.printf("[HB] OK  up=%lds  pH=%.2f  RSSI=%d  doses24h=%d/%d  bufQ=%d\n",
                      uptime, ph, WiFi.RSSI(),
                      stats.dosesOk24h, stats.dosesOk24h + stats.dosesFail24h,
                      stats.bufferQueueSize);
      } else {
        Serial.printf("[HB] OK  up=%lds  RSSI=%d  doses24h=%d/%d  bufQ=%d\n",
                      uptime, WiFi.RSSI(),
                      stats.dosesOk24h, stats.dosesOk24h + stats.dosesFail24h,
                      stats.bufferQueueSize);
      }
    } else {
      Serial.println("[HB] FAIL");
    }
  }

  // pH-Messung wird jetzt in plan_executor::checkPhSampleSchedule um :05
  // jeder geraden Stunde geschrieben (synchron zur Dosier-Sequenz).
  // Nicht mehr alle 5 Min unnötig spammen.

  // OTA-Check alle 6 h
  ota_update::tick();

  // DS3231 mit NTP-Stand updaten (max 1× pro Tag intern)
  rtc_sync::syncToRtcIfDue();

  // WLAN-Watchdog: wenn wir die Verbindung verlieren, retry
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] verloren — versuche Reconnect");
    WiFi.reconnect();
    delay(5000);
  }

  delay(50);
}
