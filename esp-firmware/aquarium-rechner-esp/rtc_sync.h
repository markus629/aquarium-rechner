// =============================================================
// DS3231-RTC-Sync — Stromausfall-Resistente Zeit
// =============================================================
// Beim Boot: RTC-Zeit (UTC) → System-Time (settimeofday)
// Bei NTP-Sync: System-Time → RTC (max 1×/24h, Wear schonen)
// Falls kein DS3231 angeschlossen: lautlos NTP-only
//
// Verkabelung: DS3231 an I²C (SDA = PIN_I2C_SDA, SCL = PIN_I2C_SCL)
// MIT CR2032-Coin-Cell → überlebt Stromausfall ~5-10 Jahre
// =============================================================
#pragma once

#include <Wire.h>
#include <RTClib.h>
#include <sys/time.h>
#include "config.h"

namespace rtc_sync {

RTC_DS3231 rtc;
bool present = false;
bool initialBootSet = false;     // wurde Zeit aus RTC gelesen?
unsigned long lastRtcWriteMs = 0;
const unsigned long RTC_WRITE_INTERVAL = 24UL * 3600UL * 1000UL;  // max 1× pro Tag

// Liest DS3231 → setzt System-Zeit. Aufrufen bevor irgendetwas Zeitkritisches läuft.
void begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!rtc.begin()) {
    Serial.println("[RTC] DS3231 nicht gefunden — laufe NTP-only (Stromausfall ohne Internet = keine Doses)");
    present = false;
    return;
  }
  present = true;
  Serial.println("[RTC] DS3231 OK");

  if (rtc.lostPower()) {
    Serial.println("[RTC] Coin-Cell leer/fehlt — DS3231 hat Power verloren. Warte auf NTP.");
    return;
  }

  DateTime n = rtc.now();
  if (n.year() < 2024 || n.year() > 2099) {
    Serial.printf("[RTC] Zeit unplausibel (%04d) — ignoriere\n", n.year());
    return;
  }

  // DS3231 hält UTC → direkt in time_t übersetzen + System-Zeit setzen
  struct timeval tv;
  tv.tv_sec = n.unixtime();
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  initialBootSet = true;
  Serial.printf("[RTC] System-Zeit aus DS3231 gesetzt: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second());
}

// Wird einmal nach erfolgreichem NTP-Sync aufgerufen — und danach max 1×/Tag.
void syncToRtcIfDue() {
  if (!present) return;
  unsigned long ms = millis();
  if (lastRtcWriteMs != 0 && ms - lastRtcWriteMs < RTC_WRITE_INTERVAL) return;
  time_t now; time(&now);
  if (now < 1700000000) return;  // NTP noch nicht synced
  rtc.adjust(DateTime((uint32_t)now));
  lastRtcWriteMs = ms;
  Serial.println("[RTC] DS3231 mit System-Zeit (NTP) synchronisiert");
}

} // namespace rtc_sync
