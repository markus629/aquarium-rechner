// =============================================================
// Status-LED — WS2812 RGB an PIN_STATUS_LED (GPIO 48)
// Über Adafruit_NeoPixel (zuverlässige Timing-Implementation)
// =============================================================
// Farb-Schema:
//   schwarz/aus = Boot vor WiFi
//   weiß        = Setup-Portal aktiv
//   blau        = verbinde / kein WiFi
//   grün        = Idle, alles OK, online
//   gelb        = Dose läuft (Plan oder Manuell)
//   cyan        = Anti-Drip Prime/Retract
//   rot         = Fehler
//   magenta     = OTA-Update läuft
// =============================================================
#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"

namespace status_led {

enum State {
  S_OFF,
  S_SETUP_PORTAL,
  S_CONNECTING,
  S_IDLE,
  S_DOSING,
  S_ANTIDRIP,
  S_ERROR,
  S_OTA
};

// 1 LED, GRB-Reihenfolge (Default für WS2812B), 800 kHz
Adafruit_NeoPixel pixel(1, PIN_STATUS_LED, NEO_GRB + NEO_KHZ800);

State current = S_OFF;
unsigned long lastBlinkMs = 0;
unsigned long lastWriteMs = 0;
bool blinkOn = true;

static void writeRGB(uint8_t r, uint8_t g, uint8_t b) {
#ifdef USE_RGB_LED
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
#endif
}

void begin() {
#ifdef USE_RGB_LED
  // WS2812 braucht beim Cold-Boot Zeit zur Stabilisierung + sauberen Reset-Pulse.
  // Datenleitung explizit LOW halten bevor wir Daten senden.
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
  delay(100);  // langer LOW = Reset-Pulse für WS2812 (>50µs nötig)

  pixel.begin();
  pixel.setBrightness(128);

  // Mehrfach initial schreiben — falls erster Frame in Reset-Phase verloren geht
  for (int i = 0; i < 5; i++) {
    pixel.clear();
    pixel.show();
    delay(5);
  }
#endif
}

void set(State s) {
  // Throttling: bei State-Wechsel sofort, sonst max 1x alle 2 Sekunden refreshen.
  // (Verhindert dass der Loop pixel.show() millionenfach pro Sekunde aufruft.)
  unsigned long now = millis();
  bool changed = (s != current);
  bool dueRefresh = (now - lastWriteMs > 2000);
  if (!changed && !dueRefresh) return;
  current = s;
  lastWriteMs = now;
  switch (s) {
    case S_OFF:          writeRGB(0, 0, 0); break;
    case S_SETUP_PORTAL: writeRGB(255, 255, 255); break;
    case S_CONNECTING:   writeRGB(0, 0, 255); break;
    case S_IDLE:         writeRGB(0, 255, 0); break;
    case S_DOSING:       writeRGB(0, 100, 255); break;   // blau (Pumpe aktiv)
    case S_ANTIDRIP:     writeRGB(0, 255, 255); break;
    case S_ERROR:        writeRGB(255, 0, 0); break;
    case S_OTA:          writeRGB(255, 0, 255); break;
  }
}

// Animationen für Pulsing/Blinking — alle 50 ms aufrufen
void tick() {
  unsigned long now = millis();
  if (current == S_CONNECTING) {
    if (now - lastBlinkMs > 600) {
      lastBlinkMs = now;
      blinkOn = !blinkOn;
      writeRGB(0, 0, blinkOn ? 255 : 0);
    }
  } else if (current == S_OTA) {
    if (now - lastBlinkMs > 200) {
      lastBlinkMs = now;
      blinkOn = !blinkOn;
      writeRGB(blinkOn ? 255 : 0, 0, blinkOn ? 255 : 0);
    }
  } else if (current == S_ERROR) {
    if (now - lastBlinkMs > 800) {
      lastBlinkMs = now;
      blinkOn = !blinkOn;
      writeRGB(blinkOn ? 255 : 30, 0, 0);
    }
  }
}

} // namespace status_led
