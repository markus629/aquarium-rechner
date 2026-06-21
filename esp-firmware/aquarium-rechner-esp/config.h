// =============================================================
// Aquarium-Rechner ESP-Firmware — zentrale Konfiguration
// =============================================================
#pragma once

// ---------- Firmware ----------
#define FW_VERSION "0.6.1"
#define FW_NAME "aquarium-rechner-esp"

// ---------- Geräte-Rolle ----------
// EINE Firmware für mehrere Geräte. Die Rolle bestimmt, welcher Dosierplan
// ausgeführt wird und welche Collections genutzt werden:
//   "kalk"     → aqua_*  (Ca/KH, pH-Sensor, Tag/Nacht)   — wie bisher
//   "nutrient" → nutri_* (NO3/PO4: N/P/C/La, pH AUS)
//   ""         → Leerlauf (nur Heartbeat, keine Dosierung)
// Die Rolle liegt in der devices-Collection (chipId→role), web-umschaltbar.
// Beim Boot wird sie aus NVS geladen (offline-fähig) und online aus der DB
// aktualisiert. Rollen-Wechsel zur Laufzeit → sauberer Reboot.
#define NVS_KEY_ROLE  "devRole"

// OTA-Quelle: GitHub Releases (public, kein Auth nötig)
// API: https://api.github.com/repos/<OWNER>/<REPO>/releases/latest
#define OTA_GITHUB_OWNER "markus629"
#define OTA_GITHUB_REPO  "aquarium-rechner"
// Asset-Name-Filter: Datei im Release muss diesen Substring enthalten (z.B. ".bin")
#define OTA_ASSET_FILTER ".bin"

// ---------- PocketBase Backend ----------
// Bewusst NUR die öffentliche Internet-Adresse (Cloudflare-Tunnel auf die eigene
// Domain) — kein lokaler LAN-Pfad. So verhält sich JEDER ESP identisch, egal ob er
// im Heimnetz des Betreibers steht oder bei einem anderen Nutzer. Damit ist sicher,
// dass der Internet-Weg funktioniert. Ohne abschließenden Slash.
#define PB_URL "https://fishtankpro.de"

// TLS-Zertifikatsprüfung (für die HTTPS-Verbindung):
//   1 = WiFiClientSecure.setInsecure() — KEINE Zertifikatsprüfung.
//       Einfachste Variante, verbindet sich garantiert. Theoretisch
//       MITM-angreifbar. Für den ersten Flash/Test empfohlen.
//   0 = Root-CA pinnen (sicherer). Dann in pb_sync.h die passende
//       Root-CA (Cloudflare-Edge nutzt Let's Encrypt / Google Trust)
//       im Platzhalter PB_ROOT_CA setzen.
#define PB_TLS_INSECURE 1

// ---------- Hardware-Pins ----------
// Schritt-Motoren — alle Signale als Block GPIO 1-2 + 4-7
// (STEP/DIR gemeinsam, ENABLE pro Pumpe getrennt)
#define PIN_STEP        1
#define PIN_DIR         2
#define PIN_ENABLE_P0   4  // Calcium
#define PIN_ENABLE_P1   5  // Magnesium
#define PIN_ENABLE_P2   6  // KH-Tag
#define PIN_ENABLE_P3   7  // KH-Nacht

// pH-Sensor — ADC1_CH9 auf ESP32-S3
#define PIN_PH_ADC     10

// I²C (DS3231 RTC)
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9

// Status-LED (optional, auf Board ggf. anpassen)
#define PIN_STATUS_LED  48  // ESP32-S3 N16R8: WS2812 RGB an IO48
#define USE_RGB_LED     1

// Stepper-Defaults (stepsPerSec, accelPerSec2) liegen in pumps.h.
// Anti-Drip-Defaults ebenfalls in pumps.h.
// Werte werden aus PocketBase-Settings gesynct und in NVS persistiert.

// ---------- Intervalle ----------
const unsigned long HEARTBEAT_INTERVAL_MS    = 30 * 1000UL;
const unsigned long COMMAND_POLL_NORMAL_MS   =  3 * 1000UL;  // 3s — Web-Commands fühlen sich live an
const unsigned long COMMAND_POLL_FAST_MS     =  1 * 1000UL;  // 1s während laufender Aktion
const unsigned long PLAN_CHECK_INTERVAL_MS   = 60 * 1000UL;
const unsigned long PH_SAMPLE_INTERVAL_MS    =       100UL;  // ADC-Sampling
const unsigned long OTA_CHECK_INTERVAL_MS    =  6 * 3600UL * 1000UL; // alle 6 h

// ---------- WiFi-Setup ----------
#define SETUP_AP_SSID   "AquariumRechner-Setup"
#define SETUP_AP_TIMEOUT_MS  (10 * 60UL * 1000UL)  // 10 Min Setup-Portal offen lassen

// ---------- NVS-Keys ----------
#define NVS_NAMESPACE "aqrechner"
#define NVS_KEY_WIFI_SSID    "wifi_ssid"
#define NVS_KEY_WIFI_PASS    "wifi_pass"
#define NVS_KEY_PB_EMAIL     "pb_email"
#define NVS_KEY_PB_PASSWORD  "pb_password"
#define NVS_KEY_BOOT_COUNT   "boot_count"

// ---------- pH-Sensor ----------
const int PH_SAMPLE_COUNT = 40;   // Ringpuffer für gleitenden Mittelwert
