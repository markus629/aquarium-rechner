// =============================================================
// Stepper-Pumpen-Steuerung
// =============================================================
// Vier Pumpen mit gemeinsamem STEP/DIR, separater ENABLE-Leitung.
// Zur Dosis-Zeit wird die ENABLE-Leitung der gewählten Pumpe LOW gezogen,
// nach Beendigung wieder HIGH.
//
// Verwendet FastAccelStepper-Lib.
// =============================================================
#pragma once

#include <FastAccelStepper.h>
#include <Preferences.h>
#include "config.h"

namespace pumps {

const int NUM_PUMPS = 4;
const int ENABLE_PINS[NUM_PUMPS] = { PIN_ENABLE_P0, PIN_ENABLE_P1, PIN_ENABLE_P2, PIN_ENABLE_P3 };
const char* PUMP_NAMES[NUM_PUMPS] = { "Calcium", "Magnesium", "KH-Tag", "KH-Nacht" };

FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *stepper = nullptr;

float mlPerStep[NUM_PUMPS] = { 0.0f, 0.0f, 0.0f, 0.0f };  // wird aus Firestore geladen
float speedML = DEFAULT_SPEED_ML;          // ml/Min
float accelML = DEFAULT_ACCELERATION_ML;   // ml/Min²

int activePump = -1;
unsigned long doseStartMs = 0;
unsigned long doseExpectedMs = 0;

// ---------- Kalibrierung aus NVS laden/speichern ----------
void loadCalibrationFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  for (int i = 0; i < NUM_PUMPS; i++) {
    String key = "mlps" + String(i);
    mlPerStep[i] = p.getFloat(key.c_str(), 0.0f);
    if (mlPerStep[i] > 0) {
      Serial.printf("[Pumps] NVS-Kalibrierung Pumpe %d: %.5f ml/Schritt\n", i, mlPerStep[i]);
    }
  }
  p.end();
}

void saveCalibrationToNVS(int pumpIdx) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return;
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  String key = "mlps" + String(pumpIdx);
  p.putFloat(key.c_str(), mlPerStep[pumpIdx]);
  p.end();
}

// Setter mit automatischer NVS-Persistenz
void setMlPerStep(int pumpIdx, float v) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return;
  if (mlPerStep[pumpIdx] == v) return;
  mlPerStep[pumpIdx] = v;
  saveCalibrationToNVS(pumpIdx);
}

// ---------- Init ----------
void begin() {
  // ENABLE-Pins als Outputs, alle HIGH (= disabled bei den meisten Treibern)
  for (int i = 0; i < NUM_PUMPS; i++) {
    pinMode(ENABLE_PINS[i], OUTPUT);
    digitalWrite(ENABLE_PINS[i], HIGH);
  }
  engine.init();
  stepper = engine.stepperConnectToPin(PIN_STEP);
  if (stepper) {
    stepper->setDirectionPin(PIN_DIR);
    stepper->setAutoEnable(false);  // wir steuern ENABLE selbst
    Serial.println("[Pumps] Stepper-Engine bereit");
  } else {
    Serial.println("[Pumps] FEHLER: stepperConnectToPin fehlgeschlagen");
  }
  loadCalibrationFromNVS();
}

void enablePump(int idx) {
  for (int i = 0; i < NUM_PUMPS; i++) {
    digitalWrite(ENABLE_PINS[i], (i == idx) ? LOW : HIGH);
  }
  activePump = idx;
}

void disableAllPumps() {
  for (int i = 0; i < NUM_PUMPS; i++) {
    digitalWrite(ENABLE_PINS[i], HIGH);
  }
  activePump = -1;
}

bool isBusy() {
  return stepper && stepper->isRunning();
}

// ---------- Umrechnung ml/min ↔ Steps/sec (1:1 aus Original-ESP-Code) ----------
// Setzt voraus dass mlPerStep[pumpIdx] > 0 (Pumpe kalibriert).
uint32_t mlPerMinToStepsPerSec(float mlPerMin, int pumpIdx) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return 0;
  if (mlPerStep[pumpIdx] <= 0.0f) return 0;
  float mlPerSec = mlPerMin / 60.0f;
  return (uint32_t)roundf(mlPerSec / mlPerStep[pumpIdx]);
}

uint32_t mlPerMin2ToStepsPerSec2(float mlPerMin2, int pumpIdx) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return 0;
  if (mlPerStep[pumpIdx] <= 0.0f) return 0;
  float mlPerSec2 = mlPerMin2 / 3600.0f;
  return (uint32_t)roundf(mlPerSec2 / mlPerStep[pumpIdx]);
}

// ---------- Schritte fahren (für Kalibrierung & Dosierung) ----------
bool runSteps(int pumpIdx, long steps) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS || !stepper) return false;
  if (isBusy()) return false;
  enablePump(pumpIdx);
  // Speed/Accel: ml/min → Steps/sec über die Kalibrierung der Pumpe
  uint32_t stepsPerSec = mlPerMinToStepsPerSec(speedML, pumpIdx);
  if (stepsPerSec < 10) stepsPerSec = 10;  // Minimum-Speed (sicher fahrbar)
  // Beschleunigung: 0 = "quasi sofort" wie im Original (sehr hohe Accel)
  uint32_t accelSteps;
  if (accelML > 0) {
    accelSteps = mlPerMin2ToStepsPerSec2(accelML, pumpIdx);
    if (accelSteps < 10) accelSteps = 10;
  } else {
    accelSteps = 10000;
  }
  stepper->setSpeedInHz(stepsPerSec);
  stepper->setAcceleration(accelSteps);
  long target = steps;  // signed: negative = rückwärts (Anti-Drip)
  stepper->move(target);
  doseStartMs = millis();
  doseExpectedMs = (uint32_t)((labs(steps) * 1000UL) / stepsPerSec);
  Serial.printf("[Pumps] %s: %ld Schritte @ %u steps/sec (accel %u) (~%lu ms)\n",
                PUMP_NAMES[pumpIdx], steps, stepsPerSec, accelSteps,
                (unsigned long)doseExpectedMs);
  return true;
}

// ---------- Dosis in ml fahren ----------
bool runMl(int pumpIdx, float ml) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return false;
  if (mlPerStep[pumpIdx] <= 0.0f) {
    Serial.printf("[Pumps] FEHLER: Pumpe %d nicht kalibriert (mlPerStep=0)\n", pumpIdx);
    return false;
  }
  long steps = (long)round(ml / mlPerStep[pumpIdx]);
  return runSteps(pumpIdx, steps);
}

// ---------- Loop-Check: Pumpe fertig? ----------
// Returns: -1 = idle, 0..3 = pump that just finished, -2 = still running
int checkAndDisable() {
  if (activePump < 0) return -1;
  if (stepper && stepper->isRunning()) return -2;
  int finishedPump = activePump;
  disableAllPumps();
  return finishedPump;
}

void emergencyStop() {
  if (stepper) stepper->forceStop();
  disableAllPumps();
  Serial.println("[Pumps] NOT-AUS");
}

} // namespace pumps
