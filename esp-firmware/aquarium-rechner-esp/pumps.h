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

// ---------- Schritte fahren (für Kalibrierung) ----------
bool runSteps(int pumpIdx, long steps) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS || !stepper) return false;
  if (isBusy()) return false;
  enablePump(pumpIdx);
  // Stepper-Speed in Steps/Sec aus ml/Min ableiten — solange wir nicht kalibriert sind,
  // verwenden wir einen Default von 800 steps/sec
  uint32_t stepsPerSec = (uint32_t)(800.0 / 60.0 * speedML);
  uint32_t accelSteps  = (uint32_t)(800.0 / 60.0 * accelML);
  if (stepsPerSec < 100) stepsPerSec = 100;
  if (accelSteps  < 100) accelSteps  = 100;
  stepper->setSpeedInHz(stepsPerSec);
  stepper->setAcceleration(accelSteps);
  long target = steps;  // signed: negative = rückwärts (Anti-Drip)
  stepper->move(target);
  doseStartMs = millis();
  // erwartete Dauer (ms)
  doseExpectedMs = (uint32_t)((labs(steps) * 1000UL) / stepsPerSec);
  Serial.printf("[Pumps] %s: %ld Schritte @ %u steps/sec (~%lu ms)\n",
                PUMP_NAMES[pumpIdx], steps, stepsPerSec, (unsigned long)doseExpectedMs);
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
