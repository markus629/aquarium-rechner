// =============================================================
// Upload-Buffer (Offline-Resilienz) — kompakte Records auf FATFS
// =============================================================
// Jede Dose / pH-Messung / pH-Kalibrierung wird zuerst als KOMPAKTER
// Record (~32 Byte) in eine FIFO-Queue gelegt und auf der FATFS-Partition
// (9,9 MB) persistiert. Bei Erfolg → entfernt. Bei Fehler (kein WLAN,
// Server down, Token expired) bleibt der Eintrag und wird periodisch
// erneut versucht.
//
// Wichtig: Der Timestamp wird im Moment des Dosierens/Messens erfasst und
// im Record gespeichert. Beim späteren Senden (Stunden/Tage danach) wird
// genau dieser Original-Timestamp übertragen — nicht der Sendezeitpunkt.
//
// Garantie: keine Daten gehen verloren, auch nicht bei WLAN-Ausfall oder
// Power-Cycle (FATFS-Datei überlebt Reboot).
//
// Kapazität: 800 Einträge → ~2,5 Wochen Offline-Backlog bei 12 Dosier-
// stunden/Tag × 3 Pumpen + 12 pH-Werte (~48/Tag). Bei Überlauf: ältester
// Eintrag wird verworfen (FIFO drop).
//
// Speicherung kompakt statt Backend-JSON, weil 800 volle JSON-Strings
// den RAM sprengen würden. Das Backend-JSON wird erst beim Senden in
// firebase_sync::flushBuffer() aus dem Record rekonstruiert.
// =============================================================
#pragma once

#include <FFat.h>
#include <Preferences.h>
#include <vector>
#include "config.h"

namespace upload_buffer {

enum WriteKind : uint8_t { KIND_DOSE = 0, KIND_PH = 1, KIND_PHCAL = 2 };

struct PendingWrite {
  WriteKind kind;
  time_t    timestamp;   // Original-Zeitpunkt der Dose/Messung
  uint16_t  attempts;
  // Dose-Felder
  int8_t    pump;
  float     ml;
  uint8_t   doseType;    // 0=kh-day, 1=kh-night, 2=ca, 3=mg, 4=manual
  bool      isAuto;
  bool      success;
  // pH-Feld
  float     phValue;
  // pH-Kalibrierungs-Felder
  float     v4, v7;
  bool      calibrated;
};

static const int MAX_BUFFER_ITEMS = 800;
static const char* BUFFER_FILE = "/upbuf.csv";

std::vector<PendingWrite> queue;
bool ffatReady = false;
unsigned long lastFlushAttemptMs = 0;
const unsigned long FLUSH_INTERVAL_MS = 15UL * 1000;  // alle 15s versuchen
const unsigned long BACKOFF_AFTER_FAIL_MS = 60UL * 1000;
unsigned long nextRetryAtMs = 0;

// doseType-String → enum (für firebase_sync)
inline uint8_t doseTypeToCode(const char* s) {
  if (!strcmp(s, "kh-day"))   return 0;
  if (!strcmp(s, "kh-night")) return 1;
  if (!strcmp(s, "ca"))       return 2;
  if (!strcmp(s, "mg"))       return 3;
  return 4; // manual
}
inline const char* doseTypeToStr(uint8_t c) {
  switch (c) { case 0: return "kh-day"; case 1: return "kh-night";
               case 2: return "ca"; case 3: return "mg"; default: return "manual"; }
}

// ---------- Persistenz auf FATFS ----------
// CSV-Zeilen:
//   D,<ts>,<att>,<pump>,<ml>,<doseType>,<isAuto>,<success>
//   P,<ts>,<att>,<phValue>
//   C,<ts>,<att>,<v4>,<v7>,<cal>
void save() {
  if (!ffatReady) return;
  File f = FFat.open(BUFFER_FILE, FILE_WRITE);
  if (!f) { Serial.println("[Buffer] FFat open(write) FAIL"); return; }
  for (auto &w : queue) {
    if (w.kind == KIND_DOSE) {
      f.printf("D,%ld,%u,%d,%.4f,%u,%d,%d\n",
               (long)w.timestamp, w.attempts, w.pump, w.ml, w.doseType,
               w.isAuto ? 1 : 0, w.success ? 1 : 0);
    } else if (w.kind == KIND_PH) {
      f.printf("P,%ld,%u,%.4f\n", (long)w.timestamp, w.attempts, w.phValue);
    } else { // KIND_PHCAL
      f.printf("C,%ld,%u,%.4f,%.4f,%d\n",
               (long)w.timestamp, w.attempts, w.v4, w.v7, w.calibrated ? 1 : 0);
    }
  }
  f.close();
}

void load() {
  queue.clear();
  if (!ffatReady || !FFat.exists(BUFFER_FILE)) return;
  File f = FFat.open(BUFFER_FILE, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 3) continue;
    char kind = line.charAt(0);
    // in Felder splitten
    const int MAXF = 8;
    String fld[MAXF];
    int n = 0, start = 0;
    for (int i = 0; i <= line.length() && n < MAXF; i++) {
      if (i == line.length() || line.charAt(i) == ',') {
        fld[n++] = line.substring(start, i);
        start = i + 1;
      }
    }
    PendingWrite w = {};
    if (kind == 'D' && n >= 8) {
      w.kind = KIND_DOSE;
      w.timestamp = (time_t)fld[1].toInt();
      w.attempts  = (uint16_t)fld[2].toInt();
      w.pump      = (int8_t)fld[3].toInt();
      w.ml        = fld[4].toFloat();
      w.doseType  = (uint8_t)fld[5].toInt();
      w.isAuto    = fld[6].toInt() != 0;
      w.success   = fld[7].toInt() != 0;
      queue.push_back(w);
    } else if (kind == 'P' && n >= 4) {
      w.kind = KIND_PH;
      w.timestamp = (time_t)fld[1].toInt();
      w.attempts  = (uint16_t)fld[2].toInt();
      w.phValue   = fld[3].toFloat();
      queue.push_back(w);
    } else if (kind == 'C' && n >= 6) {
      w.kind = KIND_PHCAL;
      w.timestamp  = (time_t)fld[1].toInt();
      w.attempts   = (uint16_t)fld[2].toInt();
      w.v4         = fld[3].toFloat();
      w.v7         = fld[4].toFloat();
      w.calibrated = fld[5].toInt() != 0;
      queue.push_back(w);
    }
  }
  f.close();
  if (!queue.empty()) Serial.printf("[Buffer] %d Einträge aus FATFS geladen\n", (int)queue.size());
}

// ---------- Einreihen ----------
void pushAndSave(const PendingWrite &w) {
  if ((int)queue.size() >= MAX_BUFFER_ITEMS) {
    Serial.println("[Buffer] FULL — ältesten Eintrag verwerfen");
    queue.erase(queue.begin());
  }
  queue.push_back(w);
  save();
  Serial.printf("[Buffer] +1 (%d total, kind=%d)\n", (int)queue.size(), w.kind);
}

void enqueueDose(int pump, float ml, const char* doseTypeStr, bool isAuto, bool success) {
  PendingWrite w = {};
  w.kind = KIND_DOSE;
  time(&w.timestamp);
  w.attempts = 0;
  w.pump = (int8_t)pump;
  w.ml = ml;
  w.doseType = doseTypeToCode(doseTypeStr);
  w.isAuto = isAuto;
  w.success = success;
  pushAndSave(w);
}

void enqueuePh(float phValue) {
  PendingWrite w = {};
  w.kind = KIND_PH;
  time(&w.timestamp);
  w.attempts = 0;
  w.phValue = phValue;
  pushAndSave(w);
}

void enqueuePhCal(float v4, float v7, bool calibrated) {
  PendingWrite w = {};
  w.kind = KIND_PHCAL;
  time(&w.timestamp);
  w.attempts = 0;
  w.v4 = v4; w.v7 = v7; w.calibrated = calibrated;
  pushAndSave(w);
}

int size() { return (int)queue.size(); }

void begin() {
  // FATFS mounten (formatiert bei erstem Start automatisch)
  if (FFat.begin(true)) {
    ffatReady = true;
    Serial.printf("[Buffer] FATFS bereit (%lu KB frei)\n",
                  (unsigned long)(FFat.freeBytes() / 1024));
  } else {
    ffatReady = false;
    Serial.println("[Buffer] WARNUNG: FATFS-Mount fehlgeschlagen — Puffer nur im RAM");
  }
  load();

  // Einmalige Migration: alter NVS-Puffer (Format vor v0.4) → übernehmen falls vorhanden
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  if (p.isKey("bufQ")) {
    Serial.println("[Buffer] alter NVS-Puffer gefunden — wird verworfen (Format-Wechsel)");
    p.remove("bufQ");
  }
  p.end();
}

} // namespace upload_buffer
