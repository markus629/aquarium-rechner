// =============================================================
// pH-Sensor (analog) mit gleitendem Mittelwert
// =============================================================
// Liest alle ~100 ms den ADC-Wert, hält Ringpuffer von 40 Samples,
// liefert auf Anfrage den Mittelwert als pH.
//
// Kalibrierung über zwei Punkte (pH 4.0 und pH 7.0):
//   voltage_pH4, voltage_pH7 — werden aus PocketBase geladen
//   pH = 7.0 + (voltage_pH7 - voltage) / (voltage_pH4 - voltage_pH7) * 3.0
// =============================================================
#pragma once

#include "config.h"

namespace ph_sensor {

float voltagePH4 = NAN;
float voltagePH7 = NAN;
bool calibrated = false;

float samples[PH_SAMPLE_COUNT];
int sampleIdx = 0;
int sampleCount = 0;
unsigned long lastSampleMs = 0;

void begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_PH_ADC, ADC_11db);  // 0..3.3 V Bereich
  for (int i = 0; i < PH_SAMPLE_COUNT; i++) samples[i] = NAN;
  sampleIdx = 0; sampleCount = 0;
}

void setCalibration(float v4, float v7) {
  voltagePH4 = v4;
  voltagePH7 = v7;
  calibrated = !isnan(v4) && !isnan(v7);
}

// Sample-Loop: einmal pro PH_SAMPLE_INTERVAL_MS aufrufen
void tick() {
  unsigned long now = millis();
  if (now - lastSampleMs < PH_SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;
  int raw = analogRead(PIN_PH_ADC);
  float voltage = raw / 4095.0f * 3.3f;
  samples[sampleIdx] = voltage;
  sampleIdx = (sampleIdx + 1) % PH_SAMPLE_COUNT;
  if (sampleCount < PH_SAMPLE_COUNT) sampleCount++;
}

float getVoltage() {
  if (sampleCount == 0) return NAN;
  float sum = 0; int n = 0;
  for (int i = 0; i < sampleCount; i++) {
    if (!isnan(samples[i])) { sum += samples[i]; n++; }
  }
  return n > 0 ? sum / n : NAN;
}

float getPH() {
  float v = getVoltage();
  if (isnan(v) || !calibrated) return NAN;
  // Linear-Interpolation zwischen pH 4 (saurer = höhere Spannung typischerweise) und pH 7
  float ph = 7.0f + (voltagePH7 - v) / (voltagePH4 - voltagePH7) * 3.0f;
  // Plausibility-Check: aquaristisch realistisch 5..10. Werte außerhalb
  // deuten auf defekte Sonde, lose Kabel oder Sonde nicht im Wasser hin.
  if (isnan(ph) || ph < 5.0f || ph > 10.0f) return NAN;
  return ph;
}

int getSampleCount() { return sampleCount; }
bool isCalibrated() { return calibrated; }

} // namespace ph_sensor
