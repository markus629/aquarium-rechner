// =============================================================
// Aquarium-Rechner ESP-Firmware — zentrale Konfiguration
// =============================================================
#pragma once

// ---------- Firmware ----------
#define FW_VERSION "0.1.1"
#define FW_NAME "aquarium-rechner-esp"

// OTA-Quelle: GitHub Releases (public, kein Auth nötig)
// API: https://api.github.com/repos/<OWNER>/<REPO>/releases/latest
#define OTA_GITHUB_OWNER "markus629"
#define OTA_GITHUB_REPO  "aquarium-rechner"
// Asset-Name-Filter: Datei im Release muss diesen Substring enthalten (z.B. ".bin")
#define OTA_ASSET_FILTER ".bin"

// ---------- Firebase Projekt ----------
#define FIREBASE_API_KEY "AIzaSyAsQ9yj2ZrT6KBzffarsFDpov4787ON-00"
#define FIREBASE_PROJECT_ID "aquarium-rechner"
#define FIREBASE_DATABASE_URL ""  // wir verwenden nur Firestore + Storage

// ---------- Hardware-Pins ----------
// Schritt-Motoren (gemeinsame STEP/DIR, separate ENABLE pro Pumpe)
#define PIN_STEP        4
#define PIN_DIR         5
#define PIN_ENABLE_P0  14  // Calcium
#define PIN_ENABLE_P1   6  // Magnesium
#define PIN_ENABLE_P2  13  // KH-Tag
#define PIN_ENABLE_P3   7  // KH-Nacht

// pH-Sensor
#define PIN_PH_ADC      1  // ADC1_CHANNEL_0 auf ESP32-S3

// I²C (DS3231 RTC)
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9

// Status-LED (optional, auf Board ggf. anpassen)
#define PIN_STATUS_LED  48  // ESP32-S3 N16R8: WS2812 RGB an IO48
#define USE_RGB_LED     1

// ---------- Stepper-Defaults ----------
const float DEFAULT_SPEED_ML        = 3.6f;   // ml/Minute
const float DEFAULT_ACCELERATION_ML = 1.8f;   // ml/Minute²

// ---------- Intervalle ----------
const unsigned long HEARTBEAT_INTERVAL_MS    = 30 * 1000UL;
const unsigned long COMMAND_POLL_NORMAL_MS   = 30 * 1000UL;
const unsigned long COMMAND_POLL_FAST_MS     =  2 * 1000UL;
const unsigned long PLAN_CHECK_INTERVAL_MS   = 60 * 1000UL;
const unsigned long PH_SAMPLE_INTERVAL_MS    =       100UL;  // ADC-Sampling
const unsigned long OTA_CHECK_INTERVAL_MS    =  6 * 3600UL * 1000UL; // alle 6 h
const unsigned long PLAN_CACHE_TTL_MS        = 25 * 3600UL * 1000UL; // 25 h offline

// ---------- WiFi-Setup ----------
#define SETUP_AP_SSID   "AquariumRechner-Setup"
#define SETUP_AP_TIMEOUT_MS  (10 * 60UL * 1000UL)  // 10 Min Setup-Portal offen lassen

// ---------- NVS-Keys ----------
#define NVS_NAMESPACE "aqrechner"
#define NVS_KEY_WIFI_SSID    "wifi_ssid"
#define NVS_KEY_WIFI_PASS    "wifi_pass"
#define NVS_KEY_FB_EMAIL     "fb_email"
#define NVS_KEY_FB_PASSWORD  "fb_password"
#define NVS_KEY_BOOT_COUNT   "boot_count"

// ---------- pH-Sensor ----------
const int PH_SAMPLE_COUNT = 40;   // Ringpuffer für gleitenden Mittelwert
