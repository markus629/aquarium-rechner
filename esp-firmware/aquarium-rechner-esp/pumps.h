// =============================================================
// Stepper-Pumpen-Steuerung
// =============================================================
// Vier Pumpen mit gemeinsamem STEP/DIR, separater ENABLE-Leitung
// pro Pumpe. Zur Dosis-Zeit wird die ENABLE-Leitung der gewählten
// Pumpe LOW gezogen, nach Beendigung wieder HIGH.
//
// FastAccelStepper-Setup: setEnablePin(PIN_UNDEFINED) + setAutoEnable(false)
// → die Lib steuert KEIN EN, das machen wir per digitalWrite() selbst.
// move() (relativ) für saubere Anti-Drip-Sequenzen ohne Position-Drift.
//
// Anti-Drip-State-Machine: PRIME (vorwärts +ml) → DOSE (vorwärts ml)
//                          → RETRACT (rückwärts −ml).
// Prime + Retract heben sich auf → effektives Fördervolumen = ml.
//
// Kalibrierung (stepsPerML) wird in NVS persistiert, geladen beim Boot.
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

// Kalibrierung: Schritte pro ml (lesbarer als ml/Schritt, besonders bei Microstepping)
float stepsPerML[NUM_PUMPS] = { 0.0f, 0.0f, 0.0f, 0.0f };  // wird aus Firestore geladen
// Schritt-Geschwindigkeit/Beschleunigung direkt in Hz — funktioniert
// auch ohne Kalibrierung (für Kalibrier-Lauf selbst).
uint32_t stepsPerSec  = 400;   // Default: 400 Hz
uint32_t accelPerSec2 = 200;   // Default: 200 Hz/s

// ---------- Anti-Drip-Settings (kommen aus Firestore via settings_cache) ----------
bool antiDripEnabled = true;
float antiDripML = 0.015f;
uint32_t antiDripStepsPerSec = 400;
uint32_t antiDripAccelPerSec2 = 1000;  // eigene Beschleunigung für Prime/Retract

// ---------- Anti-Drip State-Machine ----------
// Eine Dose wird zu einer Sequenz: PRIME → DOSE → RETRACT
// Ohne Anti-Drip: nur DOSE.
enum DosePhase { PHASE_IDLE = 0, PHASE_PRIMING, PHASE_DOSING, PHASE_RETRACTING };
struct DoseSequence {
  DosePhase phase = PHASE_IDLE;
  int pumpIdx = -1;
  long doseSteps = 0;
  long antiDripSteps = 0;
};
DoseSequence ds;

int activePump = -1;
unsigned long doseStartMs = 0;
unsigned long doseExpectedMs = 0;

// ---------- Kalibrierung aus NVS laden/speichern ----------
void loadCalibrationFromNVS() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  for (int i = 0; i < NUM_PUMPS; i++) {
    String key = "stpml" + String(i);
    stepsPerML[i] = p.getFloat(key.c_str(), 0.0f);
    if (stepsPerML[i] > 0) {
      Serial.printf("[Pumps] NVS-Kalibrierung Pumpe %d: %.2f Schritte/ml\n", i, stepsPerML[i]);
    }
  }
  p.end();
}

void saveCalibrationToNVS(int pumpIdx) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return;
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  String key = "stpml" + String(pumpIdx);
  p.putFloat(key.c_str(), stepsPerML[pumpIdx]);
  p.end();
}

// Setter mit automatischer NVS-Persistenz
void setStepsPerML(int pumpIdx, float v) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return;
  if (stepsPerML[pumpIdx] == v) return;
  stepsPerML[pumpIdx] = v;
  saveCalibrationToNVS(pumpIdx);
}

// ---------- Init ----------
// Nach bewährtem User-Beispiel: setEnablePin(PIN_UNDEFINED) + setAutoEnable(false)
// + Speed/Accel EINMAL im Init, dann nur noch bei Bedarf updaten.
void begin() {
  Serial.println("[Pumps] Konfiguriere Pins …");
  for (int i = 0; i < NUM_PUMPS; i++) {
    pinMode(ENABLE_PINS[i], OUTPUT);
    digitalWrite(ENABLE_PINS[i], HIGH);  // HIGH = deaktiviert
    Serial.printf("[Pumps] ENABLE_PIN %d (GPIO %d) konfiguriert\n", i, ENABLE_PINS[i]);
  }

  Serial.println("[Pumps] Initialisiere Stepper-Engine …");
  engine.init();
  stepper = engine.stepperConnectToPin(PIN_STEP);
  if (stepper) {
    stepper->setDirectionPin(PIN_DIR);
    stepper->setEnablePin(PIN_UNDEFINED);   // explizit: kein Library-managed EN
    stepper->setAutoEnable(false);
    stepper->setSpeedInHz(stepsPerSec);
    stepper->setAcceleration(accelPerSec2 == 0 ? 10000 : accelPerSec2);
    Serial.printf("[Pumps] Stepper-Engine bereit (STEP=GPIO%d, DIR=GPIO%d)\n", PIN_STEP, PIN_DIR);
  } else {
    Serial.println("[Pumps] FEHLER: stepperConnectToPin fehlgeschlagen");
  }
  loadCalibrationFromNVS();
}

void enablePump(int idx) {
  // Direktes Switching wie im funktionierenden User-Beispiel — keine Delays
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

// ---------- Schritte fahren (für Kalibrierung & Dosierung) ----------
// Verwendet stepsPerSec + accelPerSec2 direkt — keine ml-Konvertierung,
// funktioniert auch bei unkalibrierten Pumpen (für die Kalibrierung selbst).
bool runSteps(int pumpIdx, long steps) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS || !stepper) return false;
  if (isBusy()) return false;
  enablePump(pumpIdx);
  uint32_t spd = stepsPerSec;
  if (spd < 10) spd = 10;
  uint32_t accel = (accelPerSec2 == 0) ? 10000 : accelPerSec2;
  stepper->setSpeedInHz(spd);
  stepper->setAcceleration(accel);
  stepper->move(steps);   // relativ — Lib setzt DIR aus dem Vorzeichen
  doseStartMs = millis();
  doseExpectedMs = (uint32_t)((labs(steps) * 1000UL) / spd);
  Serial.printf("[Pumps] %s: %ld Schritte @ %u Hz (accel %u Hz/s) (~%lu ms)\n",
                PUMP_NAMES[pumpIdx], steps, spd, accel,
                (unsigned long)doseExpectedMs);
  return true;
}

// ---------- Internal: Bewegung mit eigener Geschwindigkeit starten ----------
// hz_accel = 0 → nutzt globale accelPerSec2; sonst die übergebene Beschleunigung
bool _runStepsAtSpeed(int pumpIdx, long steps, uint32_t hz, uint32_t hz_accel = 0) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS || !stepper) return false;
  if (isBusy()) return false;
  enablePump(pumpIdx);
  if (hz < 10) hz = 10;
  uint32_t accel = hz_accel > 0 ? hz_accel
                                : ((accelPerSec2 == 0) ? 10000 : accelPerSec2);
  stepper->setSpeedInHz(hz);
  stepper->setAcceleration(accel);
  stepper->move(steps);
  doseStartMs = millis();
  doseExpectedMs = (uint32_t)((labs(steps) * 1000UL) / hz);
  return true;
}

// ---------- Dosis in ml fahren (mit Anti-Drip-Sequenz wenn aktiviert) ----------
// Sequenz: PRIME (vorwärts antiDripML) → DOSE (vorwärts ml) → RETRACT (rückwärts antiDripML)
// Effektives Fördervolumen = ml (prime + retract heben sich auf).
bool runMl(int pumpIdx, float ml) {
  if (pumpIdx < 0 || pumpIdx >= NUM_PUMPS) return false;
  if (stepsPerML[pumpIdx] <= 0.0f) {
    Serial.printf("[Pumps] FEHLER: Pumpe %d nicht kalibriert (stepsPerML=0)\n", pumpIdx);
    return false;
  }
  if (ds.phase != PHASE_IDLE) {
    Serial.println("[Pumps] FEHLER: Dose-Sequenz bereits aktiv");
    return false;
  }
  ds.pumpIdx = pumpIdx;
  ds.doseSteps = (long)round(ml * stepsPerML[pumpIdx]);
  ds.antiDripSteps = antiDripEnabled
    ? (long)round(antiDripML * stepsPerML[pumpIdx])
    : 0;

  if (ds.antiDripSteps > 0) {
    // Phase 1: PRIME (vorwärts mit Anti-Drip-Speed)
    ds.phase = PHASE_PRIMING;
    Serial.printf("[Pumps] %s SEQ: prime %ld + dose %ld + retract %ld\n",
                  PUMP_NAMES[pumpIdx], ds.antiDripSteps, ds.doseSteps, ds.antiDripSteps);
    return _runStepsAtSpeed(pumpIdx, ds.antiDripSteps, antiDripStepsPerSec, antiDripAccelPerSec2);
  } else {
    // Kein Anti-Drip: direkt dose
    ds.phase = PHASE_DOSING;
    Serial.printf("[Pumps] %s DOSE: %ld Schritte (Anti-Drip aus)\n",
                  PUMP_NAMES[pumpIdx], ds.doseSteps);
    return _runStepsAtSpeed(pumpIdx, ds.doseSteps, stepsPerSec);
  }
}

// ---------- Loop-Check: Pumpe fertig? ----------
// Returns: -1 = idle, 0..3 = pump that just finished, -2 = still running
// Bei runMl-Sequenz: erst nach PHASE_RETRACTING wird "fertig" gemeldet.
int checkAndDisable() {
  if (activePump < 0) return -1;
  if (stepper && stepper->isRunning()) return -2;

  // Wenn wir in einer Sequenz sind: nächste Phase oder Ende
  if (ds.phase == PHASE_PRIMING) {
    // Prime fertig → Dose starten
    ds.phase = PHASE_DOSING;
    Serial.printf("[Pumps] %s: prime fertig → dose %ld\n", PUMP_NAMES[ds.pumpIdx], ds.doseSteps);
    _runStepsAtSpeed(ds.pumpIdx, ds.doseSteps, stepsPerSec);
    return -2;
  }
  if (ds.phase == PHASE_DOSING) {
    if (ds.antiDripSteps > 0) {
      // Dose fertig → Retract starten
      ds.phase = PHASE_RETRACTING;
      Serial.printf("[Pumps] %s: dose fertig → retract %ld\n", PUMP_NAMES[ds.pumpIdx], ds.antiDripSteps);
      _runStepsAtSpeed(ds.pumpIdx, -ds.antiDripSteps, antiDripStepsPerSec, antiDripAccelPerSec2);
      return -2;
    }
    // Kein Anti-Drip: Sequenz fertig
    int finishedPump = ds.pumpIdx;
    ds = {};  // reset
    disableAllPumps();
    return finishedPump;
  }
  if (ds.phase == PHASE_RETRACTING) {
    // Retract fertig → komplette Sequenz fertig
    int finishedPump = ds.pumpIdx;
    Serial.printf("[Pumps] %s: retract fertig — Sequenz abgeschlossen\n", PUMP_NAMES[ds.pumpIdx]);
    ds = {};
    disableAllPumps();
    return finishedPump;
  }
  // Keine Sequenz (Kalibrier-Lauf via runSteps direkt)
  int finishedPump = activePump;
  disableAllPumps();
  return finishedPump;
}

void emergencyStop() {
  if (stepper) stepper->forceStop();
  ds = {};   // Sequenz-State zurücksetzen — sonst verweigert runMl() für immer
  disableAllPumps();
  Serial.println("[Pumps] NOT-AUS");
}

} // namespace pumps
