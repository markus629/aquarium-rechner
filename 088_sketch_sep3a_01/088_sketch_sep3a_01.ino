/*
 * Dosierpumpen-Steuerung für ESP32 WROOM
 * Mit ezTime-Bibliothek für vereinfachte Zeitverwaltung
 * Steuert 4 Dosierpumpen mit Schrittmotoren (gemeinsamer STEP und DIR Pin)
 * Pumpen: Calcium, Magnesium, KH-Tag, KH-Nacht
 * 
 * Features:
 * - WebSocket-Kommunikation
 * - Adaptives Dosiersystem und Dosierplan
 * - Individuell einstellbare Verbrauchsraten für KH und Calcium
 * - Automatische Berechnung des tatsächlichen Verbrauchs
 * - Tag/Nacht-Schaltung für KH-Dosierung
 * - Getrennte Speicherung von Messungen und Dosierungen in LittleFS
 * - Vue.js basierte Benutzeroberfläche
 */

// --- ESP32 Board: esp32 by Espressif v2.0.17 ---
// --- Benötigte Bibliotheken (Arduino Library Manager): ---
//   ArduinoJson v6.21.5
//   ESP Async WebServer v3.9.2 (+ Async TCP v3.4.9)
//   FastAccelStepper v0.33.2
//   ezTime v0.8.3
//   RTClib v2.1.4

#include <Arduino.h>                // ESP32-Core
#include <Wire.h>                   // ESP32-Core (I2C für RTC)
#include <WiFi.h>                   // ESP32-Core
#include <ArduinoJson.h>            // ArduinoJson v6.21.5
#include <ESPAsyncWebServer.h>      // ESP Async WebServer v3.9.2 (enthält AsyncWebSocket)
#include <ESPmDNS.h>                // ESP32-Core (lokale Domainnamenverwaltung)
#include <WiFiUdp.h>                 // ESP32-Core (UDP für KH-Tester Kommunikation)
#include <cstddef>                  // C++ Standard (für offsetof)
#include <FastAccelStepper.h>       // FastAccelStepper v0.33.2
#include <LittleFS.h>               // ESP32-Core (Dateispeicherung)
#include <ezTime.h>                 // ezTime v0.8.3 (Zeitverwaltung/NTP)
#include <RTClib.h>                 // RTClib v2.1.4 (DS3231 RTC)
#include <ArduinoOTA.h>             // ESP32-Core (Over-The-Air Updates)
#include <vector>                   // C++ Standard
#include <map>                      // C++ Standard
#include <set>                      // C++ Standard
#include <algorithm>                // C++ Standard
#include "webpage.h"                // Projekt: HTML-Webseite
#include "vuejs.h"                  // Projekt: Vue.js offline (158KB)

// ============ PSRAM-Infrastruktur für ESP32-S3 N16R8 ============

// Custom Allocator für ArduinoJson — große Dokumente (>1KB) landen im PSRAM
struct PsramAllocator {
  void* allocate(size_t size) {
    return (psramFound() && size > 1024) ? ps_malloc(size) : malloc(size);
  }
  void deallocate(void* ptr) { free(ptr); }
  void* reallocate(void* ptr, size_t new_size) {
    return (psramFound() && new_size > 1024) ? ps_realloc(ptr, new_size) : realloc(ptr, new_size);
  }
};
using PsramJsonDocument = BasicJsonDocument<PsramAllocator>;

#include "psram_utils.h"

// ================================================================

// WLAN-Konfiguration (dynamisch aus LittleFS geladen)
String wifiSSID = "";
String wifiPassword = "";
bool wifiConfigured = false;
bool isAPMode = false;  // Flag für Access Point Modus
const char* hostname = "dosierpumpen";  // Hostname für mDNS
const char* apSSID = "Dosierpumpen-Setup";  // AP Name wenn kein WiFi konfiguriert

// OTA-Konfiguration (immer aktiv)
const char* ota_password = "dosierpumpen";  // Passwort für OTA-Updates

// ezTime Konfiguration
Timezone Europe_Berlin;  // Zeitzone für Deutschland
bool timeInitialized = false;
bool blockNtpUpdates = false;

// Blockiert Dosierplan-Berechnungen während Pumpenaktivität
bool blockCalculations = false;
// Deferred Neuberechnung: Messung gespeichert während Pumpe aktiv
bool pendingMeasurementRecalc = false;
bool pendingRecalcIsCalcium = false;
bool pendingRecalcIsKH = false;

// Async WebServer auf Port 80
AsyncWebServer server(80);

// Async WebSocket für Hauptkommunikation
AsyncWebSocket ws("/ws");

static const size_t PSRAM_CHUNK_SIZE = 4096;

// UDP-Listener für KH-Tester Kommunikation
#define KH_UDP_PORT 4210
WiFiUDP udpServer;

// =========== DEFERRED PROCESSING ===========
// Schwere Operationen werden im WS-Handler nur angefordert und in loop() ausgeführt.
// Das verhindert Task-Watchdog-Timeouts im async_tcp Task.
// =========== TASK-QUEUE ===========
// Alle schweren Operationen (LittleFS, Berechnungen, große JSON-Antworten)
// werden in eine Queue gestellt und einzeln in loop() abgearbeitet.
// Der WS-Handler packt nur Tasks rein → blockiert nie mehr als ~1ms.

enum TaskType {
  TASK_NONE = 0,
  TASK_SAVE_MEASUREMENT,        // Messwert speichern (LittleFS write + Dosierplan)
  TASK_SAVE_SETTINGS,           // Settings in LittleFS speichern
  TASK_UPDATE_DOSAGE_PLANS,     // Dosierplan neu berechnen + broadcasten
  TASK_SEND_CONSUMPTION_DATA,   // getConsumptionDataJson → client
  TASK_SEND_PH_TREND,           // getPhTrendDataJson → client
  TASK_SEND_DOSAGE_HISTORY,     // getDosageHistoryJson → client
  TASK_SEND_WATER_MEASUREMENTS, // getWaterMeasurementsJson → client
  TASK_SEND_ALL_WATER_MEASUREMENTS, // getAllWaterMeasurementsJson → client
  TASK_DELETE_MEASUREMENT,      // Messwert löschen (LittleFS) + Updates senden
  TASK_DELETE_DOSAGE,           // Dosierung löschen (LittleFS) + Updates senden
  TASK_DELETE_AUTO_KH,          // Auto-KH-Messwert löschen (LittleFS) + Updates senden
  TASK_BROADCAST_ALL_DATA,      // Messwerte + Dosierungshistorie an alle Clients
  TASK_SEND_KH_DOSAGE_PLAN,    // KH-Dosierplan → client
  TASK_SEND_CA_DOSAGE_PLAN,    // Ca-Dosierplan → client
  TASK_SEND_SYSTEM_SETTINGS,   // SystemSettings → client
  TASK_SEND_PH_MEASUREMENTS,   // getPHMeasurementsJson → client (0 = broadcast)
  TASK_SAVE_AUTO_KH,           // Auto-KH vom Tester speichern (LittleFS + broadcast)
  TASK_ADOPT_AUTO_KH,          // Auto-KH in reguläre Messung übernehmen
  TASK_CALCULATE_DOSING_FACTORS,  // Schwankungskompensation-Faktoren berechnen
  TASK_TOGGLE_DOSING_FACTORS,    // Schwankungskompensation ein/ausschalten
  TASK_RESET_DOSING_FACTORS,     // Schwankungskompensation-Faktoren zurücksetzen
};

struct TaskItem {
  TaskType type = TASK_NONE;
  uint32_t clientId = 0;        // Ziel-Client (0 = broadcast)
  int intParam1 = 0;            // z.B. index, weeks, dosageType
  int intParam2 = 0;            // z.B. zusätzliche Flags
  float floatParam = 0;         // z.B. Messwert
  bool boolParam1 = false;      // z.B. isCalcium
  bool boolParam2 = false;      // z.B. updateKH
  bool boolParam3 = false;      // z.B. updateCa
};

// Ring-Buffer Queue — maximal 16 Tasks gleichzeitig
const int TASK_QUEUE_SIZE = 16;
TaskItem taskQueue[TASK_QUEUE_SIZE];
volatile int taskQueueHead = 0;  // Nächster freier Platz (Schreiben)
volatile int taskQueueTail = 0;  // Nächster zu verarbeitender (Lesen)

bool enqueueTask(TaskItem task) {
  int nextHead = (taskQueueHead + 1) % TASK_QUEUE_SIZE;
  if (nextHead == taskQueueTail) {
    Serial.println("Task-Queue voll! Task verworfen.");
    return false;
  }
  taskQueue[taskQueueHead] = task;
  taskQueueHead = nextHead;
  return true;
}

bool hasQueuedTasks() {
  return taskQueueHead != taskQueueTail;
}

TaskItem dequeueTask() {
  TaskItem task;
  if (taskQueueHead == taskQueueTail) {
    task.type = TASK_NONE;
    return task;  // Queue leer
  }
  task = taskQueue[taskQueueTail];
  taskQueueTail = (taskQueueTail + 1) % TASK_QUEUE_SIZE;
  return task;
}

// Hilfsfunktion: Task einfach erstellen und einreihen
void queueTask(TaskType type, uint32_t clientId = 0) {
  TaskItem t;
  t.type = type;
  t.clientId = clientId;
  enqueueTask(t);
}

// --------------------------------------------------------------------------
// Chunked-Send: große JSON-Payloads (PSRAM) in kleinen Frames senden.
// Frame-Format für Chunks:
//   "CHUNK:<id>:<seq>:<total>:<rawJsonPiece>"
// Der Client konkateniert die rawJsonPieces in Reihenfolge und JSON.parse'd
// erst am Ende. Kleine Payloads (<= PSRAM_CHUNK_SIZE) werden unverändert als
// einzelne JSON-Nachricht gesendet — der UI-Client erkennt das am fehlenden
// CHUNK:-Präfix.
// clientId == 0 => Broadcast an alle Clients.
// --------------------------------------------------------------------------
void sendPsramSingle(const char* buf, size_t len, uint32_t clientId) {
  if (clientId == 0) {
    if (ws.count() > 0) ws.textAll(buf, len);
  } else {
    AsyncWebSocketClient* c = ws.client(clientId);
    if (c && c->status() == WS_CONNECTED) c->text(buf, len);
  }
}

// Kleiner, wiederverwendbarer Heap-Frame (Header + Chunk-Payload) — bei
// PSRAM_CHUNK_SIZE=4096 maximal ~4.2 KB Heap pro Frame, danach wieder frei.
void sendPsramChunked(const char* buf, size_t len, const char* id, uint32_t clientId) {
  if (len == 0) return;
  if (len <= PSRAM_CHUNK_SIZE) {
    sendPsramSingle(buf, len, clientId);
    return;
  }

  size_t total = (len + PSRAM_CHUNK_SIZE - 1) / PSRAM_CHUNK_SIZE;

  // Frame-Buffer einmal allokieren, pro Chunk wiederverwenden.
  const size_t frameCap = PSRAM_CHUNK_SIZE + 64;
  char* frame = (char*)malloc(frameCap);
  if (!frame) {
    Serial.println("[sendPsramChunked] Heap-OOM für Frame-Buffer");
    return;
  }

  for (size_t i = 0; i < total; i++) {
    size_t off = i * PSRAM_CHUNK_SIZE;
    size_t n = (i + 1 == total) ? (len - off) : PSRAM_CHUNK_SIZE;
    int hdr = snprintf(frame, frameCap, "CHUNK:%s:%u:%u:", id, (unsigned)i, (unsigned)total);
    if (hdr < 0 || (size_t)hdr + n > frameCap) break;  // Safety
    memcpy(frame + hdr, buf + off, n);
    sendPsramSingle(frame, (size_t)hdr + n, clientId);
    yield();  // TCP-Queue atmen lassen
  }

  free(frame);
}

// Abwärtskompatibel: pendingSettingsSave Flag (wird von vielen Stellen gesetzt)
struct DeferredFlags {
  volatile bool pendingSettingsSave = false;
} deferredFlags;

// Pin-Konfiguration für ESP32-S3 N16R8
// HINWEIS: GPIO 26-37 sind durch Octal-SPI PSRAM belegt und NICHT nutzbar!
const int STEP_PIN = 4;                        // Gemeinsamer STEP-Pin für alle Pumpen (IO4)
const int DIR_PIN = 5;                         // Gemeinsamer DIR-Pin für alle Pumpen (IO5)
const int ENABLE_PIN[4] = { 14, 6, 13, 7 };   // ENABLE-Pins: Ca(IO14), Mg(IO6), KH-Tag(IO13), KH-Nacht(IO7)

// Engine für FastAccelStepper
FastAccelStepperEngine engine = FastAccelStepperEngine();
// Stepper-Objekte (werden später initialisiert)
FastAccelStepper* pumpenStepper[4] = { NULL, NULL, NULL, NULL };

// Definiere Konstanten für Dosierungstypen
const int DOSAGE_TYPE_CALCIUM = 0;
const int DOSAGE_TYPE_MAGNESIUM = 1;
const int DOSAGE_TYPE_KH_DAY = 2;
const int DOSAGE_TYPE_KH_NIGHT = 3;

// Neue Pump-States für den Zustandsautomaten
enum PumpState {
  PUMP_IDLE,               // Keine Aktion
  PUMP_MANUAL_DISPENSING,  // Manuelle Dosierung läuft
  PUMP_AUTO_DISPENSING,    // Automatische Dosierung läuft
  PUMP_ANTI_DRIP_RETRACT,  // Anti-Tropf-Rückzug läuft
  PUMP_ANTI_DRIP_PRIME,    // Anti-Tropf-Vorschub (Entlüftung) läuft
  PUMP_CALIBRATING         // Kalibrierung läuft
};

// Dateipfade für LittleFS
const char* KH_MEASUREMENTS_FILE = "/kh_measurements.bin";
const char* CA_MEASUREMENTS_FILE = "/ca_measurements.bin";
const char* AUTO_KH_MEASUREMENTS_FILE = "/auto_kh_measurements.bin";  // Automatische KH-Messungen vom Tester
const char* CA_DOSAGES_FILE = "/ca_dosages.bin";
const char* MG_DOSAGES_FILE = "/mg_dosages.bin";
const char* KH_DAY_DOSAGES_FILE = "/kh_day_dosages.bin";
const char* KH_NIGHT_DOSAGES_FILE = "/kh_night_dosages.bin";
const char* SETTINGS_FILE = "/system_settings.json";
const char* PUMP_CONFIG_FILE = "/pump_config.json";
const char* DOSING_FACTORS_FILE = "/dosing_factors.json";
const char* PH_CALIBRATION_FILE = "/ph_calibration.json";
const char* KH_DOSAGE_PLAN_FILE = "/kh_dosage_plan.json";
const char* CA_DOSAGE_PLAN_FILE = "/ca_dosage_plan.json";
const char* WIFI_CONFIG_FILE = "/wifi_config.json";

// Neue Konstanten für pH-Sensor
const int PH_PIN = 1;                                       // Analoger Pin für pH-Sensor (ADC1_CH0 auf ESP32-S3, IO1)

// I2C-Pins für DS3231 RTC (ESP32-S3 Default-Pins sind anders als WROOM)
const int I2C_SDA_PIN = 8;                                  // I2C SDA (IO8)
const int I2C_SCL_PIN = 9;                                  // I2C SCL (IO9)
const float FIXED_TEMPERATURE = 25.0;                       // Konstante Temperatur in °C
const char* PH_MEASUREMENTS_FILE = "/ph_measurements.bin";  // Dateipfad für pH-Messwerte
const float PH_CALIBRATION_INTERVAL = 10000;                // 10 Sekunden für die Kalibrierungsstabilisierung
const int PH_SAMPLE_COUNT = 10;                             // Anzahl der Messungen für Durchschnitt

// RTC-Variablen und Konstanten
RTC_DS3231 rtc;
bool rtcInitialized = false;
unsigned long lastRtcNtpSync = 0;
const unsigned long RTC_NTP_SYNC_INTERVAL = 24UL * 60UL * 60UL * 1000UL;  // 1 Tag in Millisekunden (Overflow-sicher)

// pH-Messvariablen
float currentPH = 7.0;  // Aktueller pH-Wert
bool isPhCalibrating = false;
bool isPhCalibrationStable = false;
float phCalibrationValue = 0.0;
unsigned long phCalibrationStartTime = 0;
unsigned long lastPhMeasurement = 0;
float phSamples[PH_SAMPLE_COUNT];
int phSampleIndex = 0;

// Dosage history navigation variables
std::vector<String> historyDays;                    // Collection of days with dosage history data
int currentHistoryDayOffset = 0;                    // Current day offset for history navigation
String dosageHistoryDayTitle = "Alle Dosierungen";  // Title for current history day

// Zusätzliche pH-Messvariablen für verbesserte Messung
#define PH_ARRAY_LENGTH 40     // Anzahl der Messungen für Durchschnitt
int pHArray[PH_ARRAY_LENGTH];  // Speichert die Werte vom Sensor
int pHArrayIndex = 0;
float pHVoltage = 0.0;  // Aktuelle pH-Sensor Spannung

// Pumpenkonfiguration
struct PumpConfig {
  String name;                 // Name der Pumpe
  float mlPerStep;             // ml pro Schritt (Kalibrierungswert)
  float speedML;               // Geschwindigkeit (ml pro Minute)
  float accelerationML;        // Beschleunigung (ml pro Minute²)
  time_t lastCalibrationDate;  // Datum der letzten Kalibrierung als time_t (statt uint32_t)
};

// Neue Strukturen für eigene pH-Kalibrierung
struct PHCalibration {
  float voltage_pH4;     // Spannung bei pH 4
  float voltage_pH7;     // Spannung bei pH 7
  bool isCalibrated;     // Kalibrierungsstatus
  time_t timestamp_pH4;  // Zeitstempel der letzten pH 4 Kalibrierung
  time_t timestamp_pH7;  // Zeitstempel der letzten pH 7 Kalibrierung
};

// Globale pH-Kalibrierungsvariablen
PHCalibration phCal = { 0, 0, false, 0, 0 };  // Alle Felder initialisieren

// Struktur für Dosierungsaufträge
struct DosingJob {
  int pumpIndex;
  float amount;
  bool isAutomatic;
  float factor;  // Schwankungskompensation-Faktor (1.0 = kein Faktor)
};

// Neue Struktur für Messungen
// WICHTIG: uint32_t statt time_t für binäre Kompatibilität!
// ESP32 v3.3.7 hat time_t=64bit, was die Struct-Größe ändert und alte .bin-Dateien korrumpiert.
// uint32_t reicht bis 2106 und hält die Structs auf fester Größe (12/24 Bytes).
struct Measurement {
  uint32_t timestamp;  // Unix timestamp (uint32_t für feste Struct-Größe)
  float value;         // Messwert (KH oder Calcium)
  int index;           // Index für Referenzierung/Löschung
};
static_assert(sizeof(Measurement) == 12, "Measurement muss 12 Bytes sein fuer binaere Kompatibilitaet!");

// Neue Struktur für Dosierungen
struct Dosage {
  uint32_t timestamp;  // Unix timestamp (uint32_t für feste Struct-Größe)
  float amount;        // Dosiermenge in ml (bei Erhaltung: original * factor)
  int pumpIndex;       // Index der Pumpe (0-3)
  int dosageType;      // Typ der Dosierung (0-3)
  bool isAutomatic;    // true wenn automatische Dosierung
  float factor;        // Schwankungskompensation-Faktor (1.0 = kein Faktor)
  int index;           // Index für Referenzierung/Löschung
};
static_assert(sizeof(Dosage) == 28, "Dosage muss 28 Bytes sein fuer binaere Kompatibilitaet!");

// Neue Struktur für pH-Messungen
struct PhMeasurement {
  uint32_t timestamp;  // Unix timestamp (uint32_t für feste Struct-Größe)
  float value;         // pH-Wert
  int index;           // Index für Referenzierung/Löschung
};
static_assert(sizeof(PhMeasurement) == 12, "PhMeasurement muss 12 Bytes sein fuer binaere Kompatibilitaet!");

// Struktur für tägliche Verbrauchsdaten
struct DailyConsumption {
  time_t date;
  float khConsumption;  // °dKH pro Tag
  float caConsumption;  // mg/l pro 100L pro Tag
  bool hasData;         // Flag ob Daten vorhanden
};

struct SystemSettings {
  float aquariumVolume;             // Aquariumvolumen in Litern
  float targetKH;                   // Zielwert für KH
  float targetCalcium;              // Zielwert für Calcium
  int historyCount;                 // Anzahl der zu berücksichtigenden historischen Messwerte
  bool autoDosing;                  // Automatische Dosierung aktiviert
  float maxDailyChangeKH;           // Maximale tägliche KH-Änderung in °dKH
  float maxDailyChangeCalcium;      // Maximale tägliche Calcium-Änderung in mg/l
  time_t lastAutoDosage;            // Zeitstempel der letzten automatischen Dosierung
  float magnesiumRatio;             // Magnesium-Calcium Verhältnis in Prozent
  int khNightStart;                 // Startzeit der KH-Nacht-Dosierung (Stunde, 0-23)
  int khNightEnd;                   // Endzeit der KH-Nacht-Dosierung (Stunde, 0-23)
  float initialKHConsumption;       // Initiale KH-Verbrauchsrate in ml/Tag
  float initialCalciumConsumption;  // Initiale Calcium-Verbrauchsrate in ml/Tag
  bool autoUpdateInitialRates;      // Initiale Raten automatisch mit berechneten Werten aktualisieren
  float containerCapacity[4];       // Kapazität jedes Kanisters in ml
  float containerLevel[4];          // Aktueller Füllstand jedes Kanisters in ml
  time_t lastContainerRefill[4];    // Zeitstempel der letzten Kanister-Nachfüllung
  bool usePhBasedKHDosing;          // Neue Eigenschaft: pH-basierte KH-Dosierung aktiviert
  float phThresholdForKHNight;      // Neue Eigenschaft: pH-Schwellwert für KH-Nacht-Dosierung
  bool enableAntiDrip;              // Anti-Tropf-Funktion aktivieren/deaktivieren
  float antiDripML;                 // Rückzugsvolumen in ml
  float antiDripSpeedML;            // Rückzugsgeschwindigkeit in ml/min
};

// Konstanten für Datenspeicherung - nur Zeitbegrenzung
const int DATA_RETENTION_DAYS = 90;  // Alle Daten für 90 Tage aufbewahren

// Neue Strukturen für getrennte Dosierpläne
struct KHDosagePlanEntry {
  time_t date;             // Zeitpunkt der geplanten Dosierung (0 = Erhaltungsdosierung)
  float dosage;            // Geplante KH-Dosierung in ml
  float projectedValue;    // Prognostizierter KH-Wert
  bool isNightDosage;      // Kennzeichnet, ob es eine KH-Nacht-Dosierung ist
  bool isMaintenanceDose;  // Kennzeichnet, ob es die Erhaltungsdosierung ist
};

struct CaDosagePlanEntry {
  time_t date;             // Zeitpunkt der geplanten Dosierung (0 = Erhaltungsdosierung)
  float caDosage;          // Geplante Calcium-Dosierung in ml
  float mgDosage;          // Geplante Magnesium-Dosierung in ml
  float projectedCa;       // Prognostizierter Calcium-Wert
  bool isMaintenanceDose;  // Kennzeichnet, ob es die Erhaltungsdosierung ist
};

// Konstanten für die maximale Anzahl von Einträgen, mit Puffer für viele Ausgleichsdosierungen
const int MAX_KH_PLAN_ENTRIES = 100;  // Mehr als genug für Ausgleich + Erhaltungsdosis
const int MAX_CA_PLAN_ENTRIES = 50;   // Mehr als genug für Ausgleich + Erhaltungsdosis

// Globale Variablen für die Pläne
KHDosagePlanEntry khDosagePlan[MAX_KH_PLAN_ENTRIES];
CaDosagePlanEntry caDosagePlan[MAX_CA_PLAN_ENTRIES];
int khDosagePlanSize = 0;  // Tatsächliche Anzahl der Einträge im KH-Plan
int caDosagePlanSize = 0;  // Tatsächliche Anzahl der Einträge im Ca-Plan

// Standardwerte
const int DEFAULT_CALIBRATION_STEPS = 100;
const float DEFAULT_SPEED_ML = 3.6;         // 3.6 ml pro Minute (= 0.06 ml/s)
const float DEFAULT_ACCELERATION_ML = 1.8;  // 1.8 ml pro Minute² (= 0.03 ml/s²)

// Standardwerte für Systemeinstellungen
const float DEFAULT_AQUARIUM_VOLUME = 450.0;           // 450 Liter
const float DEFAULT_TARGET_KH = 7.5;                   // 7,5 °dKH
const float DEFAULT_TARGET_CALCIUM = 420.0;            // 420 mg/l
const int DEFAULT_HISTORY_COUNT = 5;                   // 5 Messungen berücksichtigen
const float DEFAULT_MAX_DAILY_CHANGE_KH = 2.0;         // 2 °dKH pro Tag
const float DEFAULT_MAX_DAILY_CHANGE_CALCIUM = 20.0;   // 20 mg/l pro Tag
const float DEFAULT_MAGNESIUM_RATIO = 50.0;            // 50% der Calcium-Dosierung
const int DEFAULT_KH_NIGHT_START = 19;                 // 19 Uhr (7 PM)
const int DEFAULT_KH_NIGHT_END = 7;                    // 7 Uhr (7 AM)
const float DEFAULT_INITIAL_KH_CONSUMPTION = 160;      // 160 ml/Tag für KH
const float DEFAULT_INITIAL_CALCIUM_CONSUMPTION = 60;  // 60 ml/Tag für Calcium
const float KH_ML_PER_DKH_100L = 20.0;                 // 20ml KH-Lösung pro 1°dKH pro 100L
const float CA_ML_PER_MGL_100L = 1.0;                  // 1ml Calcium-Lösung pro 1mg/l pro 100L
const float DEFAULT_CONTAINER_CAPACITY = 5000.0;       // 5 Liter Standardkapazität
const float DEFAULT_PH_THRESHOLD = 8.0;                // pH-Schwellwert für KH-Nacht-Dosierung

// Anti Tropf Standartwerte
const bool DEFAULT_ENABLE_ANTI_DRIP = true;    // Anti-Tropf standardmäßig aktiviert
const float DEFAULT_ANTI_DRIP_ML = 0.015;      // 0.015 ml Rückzug
const float DEFAULT_ANTI_DRIP_SPEED_ML = 3.6; // 3.6 ml/min Rückzugsgeschwindigkeit (= 0.06 ml/s)

// Warteschlange für Dosierungen (bis zu 5 gleichzeitig)
const int MAX_DOSING_QUEUE = 5;
DosingJob dosingQueue[MAX_DOSING_QUEUE];
int queueHead = 0;  // Position für nächsten Entnahme
int queueTail = 0;  // Position für nächste Hinzufügung
bool queueEmpty = true;

// Globale Variablen für die Dosierungssequenz
bool dosageSequenceActive = false;
int pendingDosages = 0;
int currentDosageHour = 0;          // Die Stunde, für die die aktuelle Sequenz läuft
time_t currentDosageTimestamp = 0;  // Zeitstempel beim Start der Sequenz

// Variablen für automatische Dosierung Timer
unsigned long lastCheckTime = 0;
const unsigned long checkInterval = 1 * 60 * 1000;  // 1 Minute in Millisekunden

// RAM-Cache für letzten Dosierungszeitstempel pro Typ (vermeidet LittleFS-Lesen in checkAndPerformAutoDosing)
// Index: DOSAGE_TYPE_CALCIUM=0, DOSAGE_TYPE_MAGNESIUM=1, DOSAGE_TYPE_KH_DAY=2, DOSAGE_TYPE_KH_NIGHT=3
time_t lastDosageTimeCache[4] = { 0, 0, 0, 0 };

// Event-Logger für Dosierungs-Debugging
const char* DOSING_EVENT_LOG = "/dosing_events.log";
const unsigned long MAX_LOG_SIZE = 200000;  // ~200KB (ca. 2000-3000 Einträge)
const unsigned long LOG_RETENTION_TIME = 2L * 24L * 60L * 60L;  // 2 Tage in Sekunden

// Statusvariablen
bool wifiConnected = false;
String systemStatus = "Initialisierung...";
bool needToStartNextJob = false;

// Variablen für Dosierplan-Neuberechnung
time_t lastPlanCalculationDay = 0;  // Datum der letzten Dosierplan-Berechnung
float lastKnownKHDosage = 0.0;      // Letzte bekannte KH-Dosierung
float lastKnownCaDosage = 0.0;      // Letzte bekannte Calcium-Dosierung
bool validDosingHistory = false;    // Wurde schon mindestens einmal dosiert?

// Variablen für Dateien aufräumen
unsigned long lastCleanupTime = 0;
const unsigned long cleanupInterval = 24UL * 60UL * 60UL * 1000UL;  // 24 Stunden in Millisekunden (Overflow-sicher)
bool historyDaysInitialized = false;

// Pumpenkonfiguration
PumpConfig pumps[4] = {
  { "Calcium", 0.0, DEFAULT_SPEED_ML, DEFAULT_ACCELERATION_ML, 0 },
  { "Magnesium", 0.0, DEFAULT_SPEED_ML, DEFAULT_ACCELERATION_ML, 0 },
  { "KH-Tag", 0.0, DEFAULT_SPEED_ML, DEFAULT_ACCELERATION_ML, 0 },
  { "KH-Nacht", 0.0, DEFAULT_SPEED_ML, DEFAULT_ACCELERATION_ML, 0 }
};

// Systemeinstellungen
SystemSettings settings = {
  DEFAULT_AQUARIUM_VOLUME,
  DEFAULT_TARGET_KH,
  DEFAULT_TARGET_CALCIUM,
  DEFAULT_HISTORY_COUNT,
  false,  // Automatische Dosierung standardmäßig deaktiviert
  DEFAULT_MAX_DAILY_CHANGE_KH,
  DEFAULT_MAX_DAILY_CHANGE_CALCIUM,
  0,  // Noch keine automatische Dosierung
  DEFAULT_MAGNESIUM_RATIO,
  DEFAULT_KH_NIGHT_START,
  DEFAULT_KH_NIGHT_END,
  DEFAULT_INITIAL_KH_CONSUMPTION,
  DEFAULT_INITIAL_CALCIUM_CONSUMPTION,
  true,  // autoUpdateInitialRates
  { DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY,
    DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY },  // containerCapacity
  { DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY,
    DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY },  // containerLevel
  { 0, 0, 0, 0 }                                               // lastContainerRefill
};

// Neue Funktion: SystemSettings als JSON speichern
bool saveSettingsToJson() {
  DynamicJsonDocument doc(2048);  // Größe anpassen je nach Bedarf

  // Allgemeine Einstellungen
  doc["aquariumVolume"] = settings.aquariumVolume;
  doc["targetKH"] = settings.targetKH;
  doc["targetCalcium"] = settings.targetCalcium;
  doc["historyCount"] = settings.historyCount;
  doc["autoDosing"] = settings.autoDosing;
  doc["maxDailyChangeKH"] = settings.maxDailyChangeKH;
  doc["maxDailyChangeCalcium"] = settings.maxDailyChangeCalcium;
  doc["lastAutoDosage"] = settings.lastAutoDosage;
  doc["magnesiumRatio"] = settings.magnesiumRatio;
  doc["khNightStart"] = settings.khNightStart;
  doc["khNightEnd"] = settings.khNightEnd;
  doc["initialKHConsumption"] = settings.initialKHConsumption;
  doc["initialCalciumConsumption"] = settings.initialCalciumConsumption;
  doc["autoUpdateInitialRates"] = settings.autoUpdateInitialRates;
  doc["usePhBasedKHDosing"] = settings.usePhBasedKHDosing;
  doc["phThresholdForKHNight"] = settings.phThresholdForKHNight;
  doc["enableAntiDrip"] = settings.enableAntiDrip;
  doc["antiDripML"] = settings.antiDripML;
  doc["antiDripSpeedML"] = settings.antiDripSpeedML;

  // Zeit-Offset hinzufügen (ersetzt den Hardcoded-Wert von 3600)
  doc["timeOffset"] = 3600;  // Standard: +1 Stunde

  // Container-Arrays
  JsonArray containerCapacityArr = doc.createNestedArray("containerCapacity");
  JsonArray containerLevelArr = doc.createNestedArray("containerLevel");
  JsonArray lastContainerRefillArr = doc.createNestedArray("lastContainerRefill");

  for (int i = 0; i < 4; i++) {
    containerCapacityArr.add(settings.containerCapacity[i]);
    containerLevelArr.add(settings.containerLevel[i]);
    lastContainerRefillArr.add(settings.lastContainerRefill[i]);
  }

  // In Datei speichern
  File file = LittleFS.open(SETTINGS_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Fehler beim Öffnen der Einstellungsdatei zum Schreiben");
    return false;
  }

  bool success = serializeJson(doc, file);
  file.close();

  Serial.println("Systemeinstellungen in JSON gespeichert");
  return success;
}

// Schwankungskompensation
float dosingFactors[12] = {1,1,1,1,1,1,1,1,1,1,1,1};  // Faktor pro 2h-Intervall (0-2h, 2-4h, ...)
float patternChangeRates[12] = {0};  // KH-Änderungsraten pro Intervall (für Mittelung mit neuem Muster)
bool dosingFactorsEnabled = false;    // Kompensation aktiv
time_t lastUsedPatternEnd = 0;        // Zeitstempel des letzten genutzten Musters
time_t lastFactorCalculation = 0;     // Zeitstempel der letzten Berechnung
bool hasNewPattern = false;           // UI: neues ungenutztes Muster vorhanden

// Schwankungskompensation-Faktoren speichern
bool saveDosingFactors() {
  DynamicJsonDocument doc(1024);
  doc["enabled"] = dosingFactorsEnabled;
  doc["lastUsedPatternEnd"] = (uint32_t)lastUsedPatternEnd;
  doc["lastCalculation"] = (uint32_t)lastFactorCalculation;

  JsonArray factorsArr = doc.createNestedArray("factors");
  JsonArray ratesArr = doc.createNestedArray("changeRates");
  for (int i = 0; i < 12; i++) {
    factorsArr.add(dosingFactors[i]);
    ratesArr.add(patternChangeRates[i]);
  }

  File file = LittleFS.open(DOSING_FACTORS_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Fehler beim Speichern der Dosing-Faktoren");
    return false;
  }
  bool success = serializeJson(doc, file);
  file.close();
  Serial.println("Schwankungskompensation-Faktoren gespeichert");
  return success;
}

// Schwankungskompensation-Faktoren laden
bool loadDosingFactors() {
  if (!LittleFS.exists(DOSING_FACTORS_FILE)) {
    Serial.println("Keine Dosing-Faktoren-Datei vorhanden, verwende Standardwerte");
    return false;
  }

  File file = LittleFS.open(DOSING_FACTORS_FILE, FILE_READ);
  if (!file) return false;

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("Fehler beim Laden der Dosing-Faktoren: ");
    Serial.println(error.c_str());
    return false;
  }

  dosingFactorsEnabled = doc["enabled"] | false;
  lastUsedPatternEnd = doc["lastUsedPatternEnd"] | 0;
  lastFactorCalculation = doc["lastCalculation"] | 0;

  JsonArray factorsArr = doc["factors"];
  JsonArray ratesArr = doc["changeRates"];
  for (int i = 0; i < 12; i++) {
    dosingFactors[i] = (factorsArr && i < factorsArr.size()) ? factorsArr[i].as<float>() : 1.0;
    patternChangeRates[i] = (ratesArr && i < ratesArr.size()) ? ratesArr[i].as<float>() : 0.0;
  }

  Serial.print("Dosing-Faktoren geladen, Kompensation: ");
  Serial.println(dosingFactorsEnabled ? "AKTIV" : "INAKTIV");
  return true;
}

// Neue Funktion: SystemSettings aus JSON laden
bool loadSettingsFromJson() {
  if (!LittleFS.exists(SETTINGS_FILE)) {
    Serial.println("Einstellungsdatei existiert nicht, verwende Standardwerte");
    // Standardwerte werden in der aufrufenden Funktion gesetzt
    return false;
  }

  File file = LittleFS.open(SETTINGS_FILE, FILE_READ);
  if (!file) {
    Serial.println("Fehler beim Öffnen der Einstellungsdatei zum Lesen");
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("Fehler beim Parsen der JSON-Einstellungen: ");
    Serial.println(error.c_str());
    return false;
  }

  // Daten in settings-Struktur übertragen mit Fallback auf Standardwerte
  settings.aquariumVolume = doc["aquariumVolume"] | DEFAULT_AQUARIUM_VOLUME;
  settings.targetKH = doc["targetKH"] | DEFAULT_TARGET_KH;
  settings.targetCalcium = doc["targetCalcium"] | DEFAULT_TARGET_CALCIUM;
  settings.historyCount = doc["historyCount"] | DEFAULT_HISTORY_COUNT;
  // weightingMethod wird aus alten Konfigurationen ignoriert
  settings.autoDosing = doc["autoDosing"] | false;
  settings.maxDailyChangeKH = doc["maxDailyChangeKH"] | DEFAULT_MAX_DAILY_CHANGE_KH;
  settings.maxDailyChangeCalcium = doc["maxDailyChangeCalcium"] | DEFAULT_MAX_DAILY_CHANGE_CALCIUM;
  settings.lastAutoDosage = doc["lastAutoDosage"] | 0;
  settings.magnesiumRatio = doc["magnesiumRatio"] | DEFAULT_MAGNESIUM_RATIO;
  settings.khNightStart = doc["khNightStart"] | DEFAULT_KH_NIGHT_START;
  settings.khNightEnd = doc["khNightEnd"] | DEFAULT_KH_NIGHT_END;
  settings.initialKHConsumption = doc["initialKHConsumption"] | DEFAULT_INITIAL_KH_CONSUMPTION;
  settings.initialCalciumConsumption = doc["initialCalciumConsumption"] | DEFAULT_INITIAL_CALCIUM_CONSUMPTION;
  settings.autoUpdateInitialRates = doc["autoUpdateInitialRates"] | true;
  settings.usePhBasedKHDosing = doc["usePhBasedKHDosing"] | false;
  settings.phThresholdForKHNight = doc["phThresholdForKHNight"] | DEFAULT_PH_THRESHOLD;
  settings.enableAntiDrip = doc["enableAntiDrip"] | DEFAULT_ENABLE_ANTI_DRIP;

  // Backward compatibility: Alte "antiDripSteps" in "antiDripML" konvertieren
  if (doc.containsKey("antiDripSteps")) {
    // Alte Version mit Steps: In ml umrechnen (Annahme: durchschnittlich 0.0003 ml/step)
    settings.antiDripML = doc["antiDripSteps"].as<int>() * 0.0003;
  } else {
    settings.antiDripML = doc["antiDripML"] | DEFAULT_ANTI_DRIP_ML;
  }

  // Backward compatibility: Alte "antiDripSpeed" in "antiDripSpeedML" konvertieren
  if (doc.containsKey("antiDripSpeed")) {
    // Alte Version mit Steps/s: In ml/s umrechnen (Annahme: durchschnittlich 0.0003 ml/step)
    settings.antiDripSpeedML = doc["antiDripSpeed"].as<int>() * 0.0003;
  } else {
    settings.antiDripSpeedML = doc["antiDripSpeedML"] | DEFAULT_ANTI_DRIP_SPEED_ML;
  }

  // Container-Arrays laden
  if (doc.containsKey("containerCapacity") && doc["containerCapacity"].is<JsonArray>()) {
    for (int i = 0; i < 4 && i < doc["containerCapacity"].size(); i++) {
      settings.containerCapacity[i] = doc["containerCapacity"][i] | DEFAULT_CONTAINER_CAPACITY;
    }
  }

  if (doc.containsKey("containerLevel") && doc["containerLevel"].is<JsonArray>()) {
    for (int i = 0; i < 4 && i < doc["containerLevel"].size(); i++) {
      settings.containerLevel[i] = doc["containerLevel"][i] | DEFAULT_CONTAINER_CAPACITY;
    }
  }

  if (doc.containsKey("lastContainerRefill") && doc["lastContainerRefill"].is<JsonArray>()) {
    for (int i = 0; i < 4 && i < doc["lastContainerRefill"].size(); i++) {
      settings.lastContainerRefill[i] = doc["lastContainerRefill"][i] | 0;
    }
  }

  Serial.println("Systemeinstellungen aus JSON geladen");
  return true;
}

// Pumpenkonfiguration als JSON speichern
bool savePumpsToJson() {
  DynamicJsonDocument doc(2048);
  JsonArray pumpsArray = doc.createNestedArray("pumps");

  for (int i = 0; i < 4; i++) {
    JsonObject pump = pumpsArray.createNestedObject();
    pump["name"] = pumps[i].name;
    pump["mlPerStep"] = pumps[i].mlPerStep;
    pump["speedML"] = pumps[i].speedML;
    pump["accelerationML"] = pumps[i].accelerationML;
    pump["lastCalibrationDate"] = pumps[i].lastCalibrationDate;
  }

  File file = LittleFS.open(PUMP_CONFIG_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Fehler beim Öffnen der Pumpenkonfigurationsdatei zum Schreiben");
    return false;
  }

  bool success = serializeJson(doc, file);
  file.close();

  Serial.println("Pumpenkonfiguration in JSON gespeichert");
  return success;
}

// Pumpenkonfiguration aus JSON laden
bool loadPumpsFromJson() {
  if (!LittleFS.exists(PUMP_CONFIG_FILE)) {
    Serial.println("Pumpenkonfigurationsdatei existiert nicht, verwende Standardwerte");

    // Standardwerte für Pumpen setzen
    for (int i = 0; i < 4; i++) {
      pumps[i].mlPerStep = 0.0;
      pumps[i].speedML = DEFAULT_SPEED_ML;
      pumps[i].accelerationML = DEFAULT_ACCELERATION_ML;
      pumps[i].lastCalibrationDate = 0;
    }

    return false;
  }

  File file = LittleFS.open(PUMP_CONFIG_FILE, FILE_READ);
  if (!file) {
    Serial.println("Fehler beim Öffnen der Pumpenkonfigurationsdatei zum Lesen");
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("Fehler beim Parsen der JSON-Pumpenkonfiguration: ");
    Serial.println(error.c_str());
    return false;
  }

  if (doc.containsKey("pumps") && doc["pumps"].is<JsonArray>()) {
    JsonArray pumpsArray = doc["pumps"];

    for (int i = 0; i < 4 && i < pumpsArray.size(); i++) {
      if (pumpsArray[i].is<JsonObject>()) {
        JsonObject pump = pumpsArray[i];

        // Standard-Namen für Pumpen
        const char* defaultNames[4] = { "Calcium", "Magnesium", "KH-Tag", "KH-Nacht" };

        pumps[i].name = pump["name"] | String(defaultNames[i]);
        pumps[i].mlPerStep = pump["mlPerStep"] | 0.0;

        // Backward compatibility: Alte "speed" in "speedML" konvertieren
        if (pump.containsKey("speed")) {
          // Alte Version mit Steps/s: In ml/s umrechnen (mit aktuellem mlPerStep oder Annahme)
          float mlPerStep = pumps[i].mlPerStep > 0 ? pumps[i].mlPerStep : 0.0003;
          pumps[i].speedML = pump["speed"].as<int>() * mlPerStep;
        } else {
          pumps[i].speedML = pump["speedML"] | DEFAULT_SPEED_ML;
        }

        // Backward compatibility: Alte "acceleration" in "accelerationML" konvertieren
        if (pump.containsKey("acceleration")) {
          // Alte Version mit Steps/s²: In ml/s² umrechnen (mit aktuellem mlPerStep oder Annahme)
          float mlPerStep = pumps[i].mlPerStep > 0 ? pumps[i].mlPerStep : 0.0003;
          pumps[i].accelerationML = pump["acceleration"].as<int>() * mlPerStep;
        } else {
          pumps[i].accelerationML = pump["accelerationML"] | DEFAULT_ACCELERATION_ML;
        }

        pumps[i].lastCalibrationDate = pump["lastCalibrationDate"] | 0;
      }
    }
  }

  Serial.println("Pumpenkonfiguration aus JSON geladen");
  return true;
}

// pH-Kalibrierung als JSON speichern
bool savePhCalibrationToJson() {
  DynamicJsonDocument doc(512);

  doc["voltage_pH4"] = phCal.voltage_pH4;
  doc["voltage_pH7"] = phCal.voltage_pH7;
  doc["isCalibrated"] = phCal.isCalibrated;
  doc["timestamp_pH4"] = (long)phCal.timestamp_pH4;  // Explizite Konvertierung
  doc["timestamp_pH7"] = (long)phCal.timestamp_pH7;  // Explizite Konvertierung

  File file = LittleFS.open(PH_CALIBRATION_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Fehler beim Öffnen der pH-Kalibrierungsdatei zum Schreiben");
    return false;
  }

  bool success = serializeJson(doc, file);
  file.close();

  Serial.println("pH-Kalibrierung in JSON gespeichert:");
  Serial.print("  pH4: ");
  Serial.print(phCal.voltage_pH4);
  Serial.print(" mV, Zeitstempel: ");
  Serial.println(phCal.timestamp_pH4);  // Nur Zeitstempel als Zahl
  Serial.print("  pH7: ");
  Serial.print(phCal.voltage_pH7);
  Serial.print(" mV, Zeitstempel: ");
  Serial.println(phCal.timestamp_pH7);  // Nur Zeitstempel als Zahl

  return success;
}

// pH-Kalibrierung aus JSON laden
bool loadPhCalibrationFromJson() {
  if (!LittleFS.exists(PH_CALIBRATION_FILE)) {
    Serial.println("pH-Kalibrierungsdatei existiert nicht, verwende Standardwerte");

    // Standardwerte setzen
    phCal.voltage_pH4 = 3000.0;  // ~3.0V für pH 4
    phCal.voltage_pH7 = 2500.0;  // ~2.5V für pH 7
    phCal.isCalibrated = false;
    phCal.timestamp_pH4 = 0;
    phCal.timestamp_pH7 = 0;

    return false;
  }

  File file = LittleFS.open(PH_CALIBRATION_FILE, FILE_READ);
  if (!file) {
    Serial.println("Fehler beim Öffnen der pH-Kalibrierungsdatei zum Lesen");
    return false;
  }

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("Fehler beim Parsen der JSON-pH-Kalibrierung: ");
    Serial.println(error.c_str());
    return false;
  }

  phCal.voltage_pH4 = doc["voltage_pH4"] | 3000.0;
  phCal.voltage_pH7 = doc["voltage_pH7"] | 2500.0;
  phCal.isCalibrated = doc["isCalibrated"] | false;

  // Explizite Konvertierung für Zeitstempel
  phCal.timestamp_pH4 = doc.containsKey("timestamp_pH4") ? (time_t)doc["timestamp_pH4"].as<long>() : 0;
  phCal.timestamp_pH7 = doc.containsKey("timestamp_pH7") ? (time_t)doc["timestamp_pH7"].as<long>() : 0;

  Serial.println("pH-Kalibrierung aus JSON geladen:");
  Serial.print("  pH4: ");
  Serial.print(phCal.voltage_pH4);
  Serial.print(" mV, Zeitstempel: ");
  Serial.println(phCal.timestamp_pH4);  // Nur Zeitstempel als Zahl
  Serial.print("  pH7: ");
  Serial.print(phCal.voltage_pH7);
  Serial.print(" mV, Zeitstempel: ");
  Serial.println(phCal.timestamp_pH7);  // Nur Zeitstempel als Zahl

  return true;
}

// Neue getrennte Dosierpläne
// Hilfsvariablen für die letzte Planberechnung
time_t lastKHPlanCalculation = 0;  // Zeitpunkt der letzten KH-Planberechnung
time_t lastCaPlanCalculation = 0;  // Zeitpunkt der letzten Ca-Planberechnung

// Arbeitsvariablen
int currentCalibrationPump = -1;
int currentCalibrationSteps = 0;
int lastPumpOperation = -1;  // Letzte verwendete Pumpe
float lastPumpAmount = 0;    // Letzte verwendete Menge

// Verbrauchsanalyse
float dailyKHConsumption = 0.0;       // Täglicher KH-Verbrauch
float dailyCalciumConsumption = 0.0;  // Täglicher Calcium-Verbrauch

// Tracking-Variable für die aktive Pumpe
int activePumpIndex = -1;
int targetSteps = 0;
PumpState pumpState = PUMP_IDLE;
float currentDosageAmount = 0.0;
bool isCalibrationRunning = false;      // Behalten für Kompatibilität
bool isDispensingRunning = false;       // Behalten für Kompatibilität
bool currentDosingIsAutomatic = false;  // Variable für automatische Dosierung
float currentDosageFactor = 1.0;       // Schwankungskompensation-Faktor für laufende Dosierung

// Die HTML-Webseite, jetzt mit Vue.js implementiert

// Funktionsdeklarationen
// Zeit-Funktionen
String formatDateTime(time_t t);
time_t getCurrentTime();

// OTA-Funktionen
void setupOTA();

// Prototypen für neue Funktionen
void startAntiDripPrime();
void startDosing();
void startAntiDripRetract();
void completeDosage();

void activatePump(int pumpIndex);

// Index-Verwaltung für LittleFS
int getNextMeasurementIndex(const char* filename);
int getNextDosageIndex(const char* filename);

// Hilfsfunktionen für Umrechnungen
float calculateKHPerML();
float calculateCaPerML();
double averageArray(int* arr, int number);
float getPHFromVoltage(float voltage);

// Messwert- und Dosage-Funktionen
Dosage* getAllKHDosages(int& count);
void saveMeasurement(float value, bool isCalcium);
void saveDosage(int pumpIndex, float amount, bool isAutomatic, float factor = 1.0);
void deleteMeasurement(int index, bool isCalcium);
void deleteDosage(int index, int dosageType);
Measurement* getAllMeasurements(bool isCalcium, int& count);
Measurement* getAllAutoKHMeasurements(int& count);
void adoptAutoKHMeasurement(int index);
Dosage* getAllDosages(int dosageType, int& count);
Dosage* getAllCalciumDosages(int& count);
void buildAllWaterMeasurementsJson(PsramPrint& out);
void buildWaterMeasurementsJson(int weeks, PsramPrint& out);
String getDosageHistoryJson();
float getLatestValue(bool isCalcium);
time_t getLastMeasurementTimestamp(bool isCalcium);
void cleanupOldData();
bool isFirstMeasurement(bool isCalcium);
void sortMeasurementsByTimestamp(Measurement* measurements, int count);
void sortDosagesByTimestamp(Dosage* dosages, int count);
String getDosageTypeName(int dosageType);
const char* getDosageFilePath(int dosageType);
bool isKHDosageType(int dosageType);
bool isCalciumDosageType(int dosageType);
String formatDateTime(time_t t);

String getConsumptionDataJson();

// Neue pH-Funktionen
PhMeasurement* getAllPHMeasurements(int& count);
void deletePHMeasurement(int index);
float getAveragePH();
float measurePH();
void savePHMeasurement(float phValue);
void startPHCalibration(float phValue);
void updatePHCalibration();
void schedulePhMeasurement();
void buildPHMeasurementsJson(PsramPrint& out);
float getLatestPHValue();

// Pumpen-Funktionen
bool calibratePump(int pumpIndex, int steps);
bool saveCalibration(int pumpIndex, float ml);
void setGlobalSettings(float speedML, float accelerationML);
void dispensePump(int pumpIndex, float ml, bool isAutomatic);
void updatePumpOperations();
void refillContainer(int containerIndex);
bool verifyCalibrationData(int pumpIndex, float expectedMlPerStep, time_t expectedDate);
bool addToDosingQueue(int pumpIndex, float amount, bool isAutomatic, float factor = 1.0);
void startNextDosingJob();

// Hilfsfunktionen für Dosierungshistorie
void extractDaysFromDosages(Dosage* dosages, int count);

// WiFi-Konfiguration
bool loadWiFiConfig();
bool saveWiFiConfig(String ssid, String password);
void startAccessPoint();

// Systemeinstellungen und Berechnungen
void loadSystemSettings();
void saveSystemSettings(float aquariumVolume, float targetKH, float targetCalcium, int historyCount,
                        float maxDailyChangeKH, float maxDailyChangeCalcium, float magnesiumRatio,
                        int khNightStart, int khNightEnd, float initialKHConsumption,
                        float initialCalciumConsumption, bool autoUpdateInitialRates,
                        float containerCapacity0, float containerCapacity1,
                        float containerCapacity2, float containerCapacity3,
                        bool usePhBasedKHDosing, float phThresholdForKHNight,
                        bool enableAntiDrip, float antiDripML, float antiDripSpeedML, int newTimeOffset);
void setAutoDosing(bool enabled);
String getSystemSettingsJson();
void calculateConsumption();
void calculateConsumptionData(DailyConsumption* consumptionArray, int& arraySize, time_t startTime, time_t endTime);
void checkAndPerformAutoDosing();
void initLastDosageTimeCache();

// Event-Logger Funktionen
void logDosingEvent(const String& event, const String& details = "");
void cleanupDosingEventLog();
String getDosingEventLog(int maxLines = 100);
void broadcastDosagePlans();
void resetSettings();
void testFileSystemAccess();
void calculateKHDosagePlan();
void calculateCaDosagePlan();
void updateDosagePlans(bool updateKH = true, bool updateCa = true);
bool saveKHDosagePlan();
bool saveCaDosagePlan();
bool loadKHDosagePlan();
bool loadCaDosagePlan();
String getKHDosagePlanJson(int dayOffset = 0);
String getCaDosagePlanJson(int dayOffset = 0);
// String getCombinedDosagePlanJson(int dayOffset = 0);
void handleNewMeasurement(bool isCalcium);

// Kommunikation
String getPumpStatusJson();
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleRoot(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request);
void setupWebHandlers();

// Zeit-Funktionen mit ezTime

// Hilfsfunktionen für ml/s <-> Schritte/s Umrechnung
// Konvertiert ml/min zu Schritte/s (Motor arbeitet mit Schritte/s)
int mlPerMinToStepsPerSec(float mlPerMin, int pumpIndex) {
  if (pumps[pumpIndex].mlPerStep <= 0) return 0;  // Nicht kalibriert
  float mlPerSec = mlPerMin / 60.0;  // ml/min → ml/s
  return round(mlPerSec / pumps[pumpIndex].mlPerStep);
}

// Konvertiert ml/min² zu Schritte/s²
int mlPerMin2ToStepsPerSec2(float mlPerMin2, int pumpIndex) {
  if (pumps[pumpIndex].mlPerStep <= 0) return 0;  // Nicht kalibriert
  float mlPerSec2 = mlPerMin2 / 3600.0;  // ml/min² → ml/s² (60²)
  return round(mlPerSec2 / pumps[pumpIndex].mlPerStep);
}

int mlToSteps(float ml, int pumpIndex) {
  if (pumps[pumpIndex].mlPerStep <= 0) return 0;  // Nicht kalibriert
  return round(ml / pumps[pumpIndex].mlPerStep);
}

// Formatiert einen Zeitstempel in ein lesbares Format (DD.MM.YY HH:MM)
String formatDateTime(time_t t) {
  if (t == 0) return "Nie";
  return Europe_Berlin.dateTime(t, UTC_TIME, "d.m.y H:i");
}

// Neue Variable für TimeOffset
int timeOffset = 3600;  // Standard: +1 Stunde

// Holt die aktuelle Lokalzeit (UTC + timeOffset).
// RTC und Time Library speichern UTC. Der Offset wird hier einmalig addiert.
time_t getCurrentTime() {
  // Primär RTC verwenden, wenn initialisiert
  if (rtcInitialized) {
    DateTime rtcNow = rtc.now();

    // RTC liefert UTC → Umwandeln in time_t und Offset addieren
    tmElements_t tm;
    tm.Year = rtcNow.year() - 1970;
    tm.Month = rtcNow.month();
    tm.Day = rtcNow.day();
    tm.Hour = rtcNow.hour();
    tm.Minute = rtcNow.minute();
    tm.Second = rtcNow.second();

    return makeTime(tm) + timeOffset;
  }

  // Fallback auf ezTime mit Offset
  return now() + timeOffset;
}

// Neue Funktion zum Laden des Zeitoffsets
void loadTimeOffset() {
  if (LittleFS.exists(SETTINGS_FILE)) {
    File file = LittleFS.open(SETTINGS_FILE, FILE_READ);
    if (file) {
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, file);
      file.close();

      if (!error && doc.containsKey("timeOffset")) {
        timeOffset = doc["timeOffset"];
        Serial.print("Zeit-Offset aus Einstellungen geladen: ");
        Serial.print(timeOffset);
        Serial.println(" Sekunden");
      }
    }
  }
}

// Neue Funktion zum Speichern des Zeitoffsets
void saveTimeOffset(int newOffset) {
  timeOffset = newOffset;

  // Bestehende Einstellungen laden
  if (LittleFS.exists(SETTINGS_FILE)) {
    File file = LittleFS.open(SETTINGS_FILE, FILE_READ);
    if (file) {
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, file);
      file.close();

      if (!error) {
        // Offset aktualisieren
        doc["timeOffset"] = timeOffset;

        // Zurück in die Datei schreiben
        file = LittleFS.open(SETTINGS_FILE, FILE_WRITE);
        if (file) {
          serializeJson(doc, file);
          file.close();
          Serial.print("Zeit-Offset gespeichert: ");
          Serial.print(timeOffset);
          Serial.println(" Sekunden");
        }
      }
    }
  }
}

// Hilfsfunktion zum Formatieren der RTC-Zeit
String formatRTCDateTime(DateTime dt) {
  char buf[20];
  sprintf(buf, "%02d.%02d.%04d %02d:%02d:%02d", dt.day(), dt.month(), dt.year(), dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

// Universelle Funktion zur Synchronisierung der Zeit mit NTP (RTC oder Time Library)
// WICHTIG: RTC speichert immer UTC. Der timeOffset wird nur in getCurrentTime() angewendet.
void syncTimeFromNTP() {
  if (timeStatus() != timeSet) {
    Serial.println("NTP-Zeit noch nicht verfügbar");
    return;
  }

  time_t ntpTime = now();  // UTC-Zeit von ezTime (OHNE Offset — RTC speichert UTC)

  Serial.println("=== Zeit von NTP synchronisieren ===");
  Serial.print("NTP-Zeit (UTC): ");
  Serial.println(formatDateTime(ntpTime));

  if (rtcInitialized) {
    // RTC vorhanden → UTC-Zeit in RTC schreiben
    DateTime rtcOld = rtc.now();

    rtc.adjust(DateTime(year(ntpTime), month(ntpTime), day(ntpTime),
                        hour(ntpTime), minute(ntpTime), second(ntpTime)));

    DateTime rtcNew = rtc.now();

    Serial.print("  Alte RTC-Zeit: ");
    Serial.println(formatRTCDateTime(rtcOld));
    Serial.print("  ✓ Neue RTC-Zeit (UTC): ");
    Serial.println(formatRTCDateTime(rtcNew));
  } else {
    // Keine RTC → Time Library ist bereits von ezTime gesetzt
    Serial.println("  ✓ Time Library bereits von ezTime synchronisiert");
  }

  timeInitialized = true;
  lastRtcNtpSync = millis();
}

// RTC-Setup-Funktion
void setupRTC() {
  // I2C explizit mit S3-Pins initialisieren (WROOM hatte GPIO 21/22 als Default)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // RTC initialisieren
  Serial.println("Debug: Initialisiere DS3231 RTC...");
  if (!rtc.begin()) {
    Serial.println("Debug: Konnte DS3231 RTC nicht finden! System läuft ohne RTC.");
  } else {
    rtcInitialized = true;
    Serial.println("Debug: DS3231 RTC gefunden und initialisiert");

    // Wenn RTC die Zeit verloren hat oder noch nicht gesetzt wurde
    if (rtc.lostPower()) {
      Serial.println("Debug: RTC hat die Zeit verloren! Setze auf Kompilierzeit.");
      // Setze RTC auf Kompilierzeit als Fallback
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    // Debug-Ausgabe der RTC-Zeit
    DateTime now = rtc.now();
    float temp = rtc.getTemperature();
    Serial.print("Debug: RTC-Zeit: ");
    Serial.println(formatRTCDateTime(now));
    Serial.print("Debug: RTC-Temperatur: ");
    Serial.print(temp);
    Serial.println("°C");
  }
}

// Funktion zur regelmäßigen Zeit-Synchronisierung mit NTP
void checkTimeSync(unsigned long currentMillis) {
  // Zeit mit NTP synchronisieren (einmal täglich oder bei Systemstart)
  // Funktioniert mit UND ohne RTC!
  if (timeStatus() == timeSet && !blockNtpUpdates && (lastRtcNtpSync == 0 || currentMillis - lastRtcNtpSync > RTC_NTP_SYNC_INTERVAL)) {
    Serial.println("Plane NTP-Synchronisierung...");
    syncTimeFromNTP();
  }
}

// Universelle Funktion zum Setzen der Zeit von Browser (RTC oder Time Library)
// WICHTIG: browserTime ist UTC (Date.now()/1000 vom Browser). RTC speichert UTC.
// Der timeOffset wird nur in getCurrentTime() angewendet.
bool setTimeFromBrowser(time_t browserTime) {
  if (browserTime <= 0) {
    Serial.println("FEHLER: Ungültige Browser-Zeit");
    return false;
  }

  Serial.println("=== Zeit von Browser synchronisieren ===");
  Serial.print("Browser-Zeit (UTC): ");
  Serial.println(formatDateTime(browserTime));

  if (rtcInitialized) {
    // RTC vorhanden → UTC-Zeit in RTC schreiben
    rtc.adjust(DateTime(year(browserTime), month(browserTime), day(browserTime),
                        hour(browserTime), minute(browserTime), second(browserTime)));
    Serial.print("✓ RTC gesetzt (UTC): ");
    Serial.println(formatRTCDateTime(rtc.now()));
  } else {
    // Keine RTC → Time Library setzen
    setTime(browserTime);
    Serial.print("✓ Time Library gesetzt (UTC): ");
    Serial.println(formatDateTime(now()));
  }

  // Zeit ist jetzt initialisiert!
  timeInitialized = true;
  systemStatus = "Zeit mit Browser synchronisiert";

  return true;
}

// Prüft, ob zwei Zeitstempel den gleichen Tag darstellen
bool isSameDay(time_t time1, time_t time2) {
  tmElements_t tm1, tm2;
  breakTime(Europe_Berlin.tzTime(time1, UTC_TIME), tm1);
  breakTime(Europe_Berlin.tzTime(time2, UTC_TIME), tm2);

  return (tm1.Year == tm2.Year && tm1.Month == tm2.Month && tm1.Day == tm2.Day);
}

// Erzeugt einen Zeitstempel für eine bestimmte Stunde an einem bestimmten Tag
time_t getTimeForHourOnDay(time_t baseTime, int daysToAdd, int hour) {
  // Zuerst den Tag anpassen
  time_t newTime = baseTime + (daysToAdd * SECS_PER_DAY);

  // Dann die Stunde setzen
  tmElements_t tm;
  breakTime(Europe_Berlin.tzTime(newTime, UTC_TIME), tm);
  tm.Hour = hour;
  tm.Minute = 10;  // Geändert auf 10 Minuten nach der vollen Stunde
  tm.Second = 0;

  return makeTime(tm);
}

// Gibt den Anfang des Tages (00:00 Uhr) zurück
time_t getStartOfDay(time_t t) {
  tmElements_t tm;
  breakTime(Europe_Berlin.tzTime(t, UTC_TIME), tm);
  tm.Hour = 0;
  tm.Minute = 0;
  tm.Second = 0;

  return makeTime(tm);
}

// Extrahiert die Stunde aus einem Zeitstempel
int getHourFromTime(time_t t) {
  tmElements_t tm;
  breakTime(Europe_Berlin.tzTime(t, UTC_TIME), tm);
  return tm.Hour;
}

// =========== INDEX-VERWALTUNG FÜR LittleFS ===========

// Hilfsfunktionen für Index-Verwaltung
int getNextMeasurementIndex(const char* filename) {
  int maxIndex = -1;
  File file = LittleFS.open(filename, FILE_READ);

  if (file) {
    while (file.available() >= sizeof(Measurement)) {
      Measurement measurement;
      file.read((uint8_t*)&measurement, sizeof(Measurement));
      if (measurement.index > maxIndex) {
        maxIndex = measurement.index;
      }
    }
    file.close();
  }

  return maxIndex + 1;
}

int getNextDosageIndex(const char* filename) {
  int maxIndex = -1;
  File file = LittleFS.open(filename, FILE_READ);

  if (file) {
    while (file.available() >= sizeof(Dosage)) {
      Dosage dosage;
      file.read((uint8_t*)&dosage, sizeof(Dosage));
      if (dosage.index > maxIndex) {
        maxIndex = dosage.index;
      }
    }
    file.close();
  }

  return maxIndex + 1;
}

// =========== MESSWERT- UND DOSAGE-FUNKTIONEN ===========

// =========== pH-MESSUNGS- UND KALIBRIERUNGSFUNKTIONEN ===========

// Kontinuierliche pH-Spannung Aktualisierung
void updatePhVoltage() {
  static unsigned long lastSamplingTime = 0;
  unsigned long currentMillis = millis();

  // Abtastung alle 20ms
  if (currentMillis - lastSamplingTime > 20) {
    // Füge neueste Messung zum zirkulären Puffer hinzu
    pHArray[pHArrayIndex++] = analogRead(PH_PIN);

    // Setze Index zurück, wenn wir das Ende des Arrays erreichen
    if (pHArrayIndex == PH_ARRAY_LENGTH) pHArrayIndex = 0;

    // Berechne Spannung mit der Durchschnittsfunktion (filtert Ausreißer)
    int avgReading = averageArray(pHArray, PH_ARRAY_LENGTH);
    pHVoltage = avgReading * 3300.0 / 4095;

    lastSamplingTime = currentMillis;
  }
}

// Funktion zur Berechnung des Durchschnitts mit Filterung von Ausreißern
double averageArray(int* arr, int number) {
  int i;
  int max, min;
  double avg;
  long amount = 0;

  if (number <= 0) {
    Serial.println("Error number for the array to averaging!");
    return 0;
  }

  if (number < 5) {  // Weniger als 5, direkter Durchschnitt
    for (i = 0; i < number; i++) {
      amount += arr[i];
    }
    avg = amount / number;
    return avg;
  } else {
    // Finde initial min/max
    if (arr[0] < arr[1]) {
      min = arr[0];
      max = arr[1];
    } else {
      min = arr[1];
      max = arr[0];
    }

    // Füge alle Werte außer min/max hinzu
    for (i = 2; i < number; i++) {
      if (arr[i] < min) {
        amount += min;  // Füge alten min-Wert hinzu
        min = arr[i];   // Dies ist der neue min-Wert
      } else if (arr[i] > max) {
        amount += max;  // Füge alten max-Wert hinzu
        max = arr[i];   // Dies ist der neue max-Wert
      } else {
        amount += arr[i];  // Füge Werte zwischen min und max hinzu
      }
    }

    // Ersten zwei Werte + entfernter min und max = muss 2 abziehen
    avg = (double)amount / (number - 2);
  }

  return avg;
}

// Funktion für Live-Spannungsmessung
String getPhVoltageLiveJson() {
  DynamicJsonDocument doc(256);

  // Aktuelle ADC-Werte und Spannung
  int rawADC = averageArray(pHArray, PH_ARRAY_LENGTH);

  // Aktuelle Spannung
  doc["rawVoltage"] = pHVoltage;
  doc["rawADC"] = rawADC;

  // Umrechnung in pH-Wert für die Anzeige - eigene Funktion statt ph.readPH verwenden
  float currentPhReading = getPHFromVoltage(pHVoltage);
  if (currentPhReading >= 0 && currentPhReading <= 14) {
    doc["currentPH"] = currentPhReading;
  } else {
    doc["currentPH"] = "Fehler";
  }

  doc["type"] = "phVoltage";  // Typ für Frontend-Erkennung

  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

// pH-Wert messen
float measurePH() {
  float voltage = pHVoltage;  // Nutze die bereits gefilterte Spannung
  float phValue = getPHFromVoltage(voltage);

  // Wert validieren
  if (phValue < 0 || phValue > 14) {
    Serial.println("Ungültiger pH-Wert gemessen, ignoriere...");
    return -1;  // Ungültiger Wert
  }

  // Debug-Ausgabe
  Serial.print("Gemessener pH-Wert: ");
  Serial.print(phValue, 2);
  Serial.print(" (bei ");
  Serial.print(voltage, 1);
  Serial.print(" mV und ");
  Serial.print(FIXED_TEMPERATURE);
  Serial.println("°C)");

  return phValue;
}

// Durchschnittliche pH-Messung erhalten
float getAveragePH() {
  // Wir nutzen jetzt den kontinuierlich aktualisierten Spannungswert
  float voltage = pHVoltage;
  float phValue = getPHFromVoltage(voltage);

  // Debug Ausgabe
  Serial.print("Durchschnittlicher pH-Wert: ");
  Serial.print(phValue, 2);
  Serial.print(" (bei ");
  Serial.print(voltage, 1);
  Serial.println(" mV)");

  return phValue;
}

// Nächsten freien Index für pH-Messungen holen
int getNextPHMeasurementIndex() {
  int maxIndex = -1;
  File file = LittleFS.open(PH_MEASUREMENTS_FILE, FILE_READ);

  if (file) {
    while (file.available() >= sizeof(PhMeasurement)) {
      PhMeasurement measurement;
      file.read((uint8_t*)&measurement, sizeof(PhMeasurement));
      if (measurement.index > maxIndex) {
        maxIndex = measurement.index;
      }
    }
    file.close();
  }

  return maxIndex + 1;
}

// pH-Messung speichern
void savePHMeasurement(float phValue) {
  if (phValue < 0 || phValue > 14) return;  // Ungültiger Wert

  // Neue Messung erstellen
  PhMeasurement newMeasurement;
  newMeasurement.timestamp = getCurrentTime();
  newMeasurement.value = phValue;

  // Bestimme nächsten freien Index
  int nextIndex = getNextPHMeasurementIndex();
  newMeasurement.index = nextIndex;

  // In LittleFS speichern
  File file = LittleFS.open(PH_MEASUREMENTS_FILE, FILE_APPEND);
  if (file) {
    file.write((uint8_t*)&newMeasurement, sizeof(PhMeasurement));
    file.close();
    Serial.print("pH-Messwert gespeichert: ");
    Serial.print(phValue);
    Serial.print(" (bei konstanter Temperatur: ");
    Serial.print(FIXED_TEMPERATURE);
    Serial.print("°C), Index: ");
    Serial.println(nextIndex);
  } else {
    Serial.println("Fehler beim Öffnen der pH-Messwertdatei");
  }
}

// Alle pH-Messungen holen
PhMeasurement* getAllPHMeasurements(int& count) {
  PhMeasurement* m = readValidEntries<PhMeasurement>(PH_MEASUREMENTS_FILE, count);
  if (m == nullptr) return psram_new_array<PhMeasurement>(0);
  return m;
}

// pH-Messung löschen (Tombstone: timestamp=0 in-place überschreiben)
void deletePHMeasurement(int index) {
  if (tombstoneByIndex<PhMeasurement>(PH_MEASUREMENTS_FILE, index)) {
    Serial.print("pH-Messwert mit Index ");
    Serial.print(index);
    Serial.println(" gelöscht (tombstone)");
  } else {
    Serial.print("pH-Messwert mit Index ");
    Serial.print(index);
    Serial.println(" nicht gefunden");
  }
}

// Neuesten pH-Wert holen (Seek-from-End, O(1) im Normalfall)
float getLatestPHValue() {
  PhMeasurement latest;
  if (readLatestEntry<PhMeasurement>(PH_MEASUREMENTS_FILE, latest)) return latest.value;
  return 7.0;  // Standardwert
}

// pH-Wert aus Spannung berechnen
float getPHFromVoltage(float voltage) {
  if (phCal.isCalibrated) {
    // Lineare Interpolation zwischen den beiden Kalibrierungspunkten
    float slope = (7.0 - 4.0) / (phCal.voltage_pH7 - phCal.voltage_pH4);
    float pH = 7.0 - (slope * (phCal.voltage_pH7 - voltage));

    return pH;
  } else {
    // Standardformel verwenden, wenn nicht kalibriert
    return 3.5 * (voltage / 1000.0);
  }
}

// Speichert einen Kalibrierungspunkt
void savePHCalibrationPoint(float phValue, float voltage) {
  // Aktuelle Zeit für Zeitstempel holen
  time_t currentTime = getCurrentTime();

  Serial.print("Speichere pH-Kalibrierungspunkt: pH ");
  Serial.print(phValue, 1);
  Serial.print(" = ");
  Serial.print(voltage, 1);
  Serial.print(" mV, Zeitstempel: ");
  Serial.print(currentTime);
  Serial.print(" (");
  Serial.print(formatDateTime(currentTime));
  Serial.println(")");

  if (phValue == 4.0) {
    phCal.voltage_pH4 = voltage;
    phCal.timestamp_pH4 = currentTime;
    Serial.print("pH 4 Kalibrierung gespeichert: ");
    Serial.print(voltage);
    Serial.print(" mV am ");
    Serial.println(formatDateTime(currentTime));
  } else if (phValue == 7.0) {
    phCal.voltage_pH7 = voltage;
    phCal.timestamp_pH7 = currentTime;
    Serial.print("pH 7 Kalibrierung gespeichert: ");
    Serial.print(voltage);
    Serial.print(" mV am ");
    Serial.println(formatDateTime(currentTime));
  }

  // Beide Kalibrierungspunkte müssen vorhanden sein
  if (phCal.voltage_pH4 > 0 && phCal.voltage_pH7 > 0) {
    phCal.isCalibrated = true;
    Serial.println("pH-Kalibrierung vollständig (pH4 und pH7 kalibriert)");
  }

  // Speichern in JSON
  bool saved = savePhCalibrationToJson();
  if (!saved) {
    Serial.println("WARNUNG: Speichern der pH-Kalibrierung fehlgeschlagen!");
  }
}

// pH-Kalibrierungsdaten laden
void loadPHCalibration() {
  // Versuch aus JSON zu laden
  if (loadPhCalibrationFromJson()) {
    Serial.println("pH-Kalibrierungsdaten aus JSON geladen:");
  } else {
    // Standardwerte werden bereits in loadPhCalibrationFromJson gesetzt
    Serial.println("Keine gültigen pH-Kalibrierungsdaten gefunden, verwende Standardwerte");
  }

  Serial.print("pH 4: ");
  Serial.print(phCal.voltage_pH4);
  Serial.print(" mV (kalibriert am: ");
  if (phCal.timestamp_pH4 > 0) {
    Serial.print(formatDateTime(phCal.timestamp_pH4));
  } else {
    Serial.print("Nie");
  }
  Serial.print("), pH 7: ");
  Serial.print(phCal.voltage_pH7);
  Serial.print(" mV (kalibriert am: ");
  if (phCal.timestamp_pH7 > 0) {
    Serial.print(formatDateTime(phCal.timestamp_pH7));
  } else {
    Serial.print("Nie");
  }
  Serial.println(")");
}

// pH-Sensor initialisieren
void setupPHSensor() {
  // ADC auf 12-Bit setzen (ESP32-S3 Default ist 13-Bit, WROOM war 12-Bit)
  analogReadResolution(12);

  // pH-Sensor-Pin
  pinMode(PH_PIN, INPUT);

  // pH-Kalibrierungsdaten laden
  loadPHCalibration();

  // pH-Puffer initialisieren
  for (int i = 0; i < PH_ARRAY_LENGTH; i++) {
    pHArray[i] = 0;
  }
}

// pH-Kalibrierung starten
void startPHCalibration(float phValue) {
  if (phValue != 4.0 && phValue != 7.0) {
    Serial.print("WARNUNG: Nur pH 4.0 und 7.0 für Kalibrierung unterstützt, nicht ");
    Serial.println(phValue);
    return;
  }

  isPhCalibrating = true;
  isPhCalibrationStable = false;
  phCalibrationValue = phValue;
  phCalibrationStartTime = millis();
  phSampleIndex = 0;

  // Initialisiere Samples mit 0
  for (int i = 0; i < PH_SAMPLE_COUNT; i++) {
    phSamples[i] = 0;
  }

  Serial.print("pH-Kalibrierung gestartet für pH ");
  Serial.println(phValue);

  // Feedback an Client senden
  DynamicJsonDocument responseDoc(256);
  responseDoc["type"] = "info";
  responseDoc["message"] = "Kalibrierung für pH " + String(phValue, 2) + " wurde gestartet. Bitte warten...";
  responseDoc["phCalibrationStatus"] = "running";
  responseDoc["phCalibrationValue"] = phValue;

  String responseJson;
  serializeJson(responseDoc, responseJson);
  ws.textAll(responseJson);
}

// pH-Kalibrierung aktualisieren
void updatePHCalibration() {
  if (!isPhCalibrating) return;

  // Zeit seit Beginn der Kalibrierung
  unsigned long elapsedTime = millis() - phCalibrationStartTime;

  // Fortschrittsberechnung (0-100%)
  int progressPercent = min(100, (int)(elapsedTime * 100 / PH_CALIBRATION_INTERVAL));

  // Alle 500ms eine Fortschrittsmeldung senden
  static unsigned long lastProgressUpdate = 0;
  if (millis() - lastProgressUpdate > 500) {
    // Fortschrittsmeldung an Client senden
    DynamicJsonDocument progressDoc(256);
    progressDoc["type"] = "phCalibrationProgress";
    progressDoc["progress"] = progressPercent;
    progressDoc["phValue"] = phCalibrationValue;

    // Zusätzlich aktuelle Spannungswerte senden für Live-Anzeige
    progressDoc["voltage"] = pHVoltage;
    progressDoc["rawADC"] = averageArray(pHArray, PH_ARRAY_LENGTH);

    String progressJson;
    serializeJson(progressDoc, progressJson);
    ws.textAll(progressJson);

    lastProgressUpdate = millis();
  }

  // Messung für gleitenden Durchschnitt speichern
  phSamples[phSampleIndex] = pHVoltage;  // Wir speichern die Spannung
  phSampleIndex = (phSampleIndex + 1) % PH_SAMPLE_COUNT;

  // Prüfen ob pH-Wert stabil ist (nach Wartezeit)
  if (elapsedTime > PH_CALIBRATION_INTERVAL) {
    // Berechne Durchschnitt der Spannung
    float sumVoltage = 0;
    for (int i = 0; i < PH_SAMPLE_COUNT; i++) {
      sumVoltage += phSamples[i];
    }

    float averageVoltage = sumVoltage / PH_SAMPLE_COUNT;

    // Standardabweichung berechnen
    float sumDiff = 0;
    for (int i = 0; i < PH_SAMPLE_COUNT; i++) {
      float diff = phSamples[i] - averageVoltage;
      sumDiff += diff * diff;
    }

    float stdDev = sqrt(sumDiff / PH_SAMPLE_COUNT);

    // Wenn Standardabweichung klein genug, ist der Wert stabil
    if (stdDev < 10) {  // 10mV als Stabilitätskriterium für die Spannung
      isPhCalibrationStable = true;

      // Speichere Kalibrierungspunkt
      savePHCalibrationPoint(phCalibrationValue, averageVoltage);

      // Erfolgsmeldung an Client senden
      DynamicJsonDocument successDoc(256);
      successDoc["type"] = "success";
      successDoc["message"] = "pH " + String(phCalibrationValue, 2) + " Kalibrierung erfolgreich abgeschlossen.";
      successDoc["phCalibrationStatus"] = "completed";
      successDoc["phCalibrationValue"] = phCalibrationValue;
      successDoc["calibratedVoltage"] = averageVoltage;

      String successJson;
      serializeJson(successDoc, successJson);
      ws.textAll(successJson);

      // Kalibrierung beenden
      isPhCalibrating = false;
    }
  }
}

// pH-Wert zur geplanten Zeit messen (10 Minuten vor jeder geraden Stunde)
void schedulePhMeasurement() {
  if (!timeInitialized) return;  // Nur wenn Zeit synchronisiert ist

  time_t now = getCurrentTime();
  int currentHour = Europe_Berlin.hour(now);
  int currentMinute = Europe_Berlin.minute(now);

  // Prüfen ob wir 5 Minuten vor einer Dosierung sind (XX:05 für gerade Stunden)
  bool isScheduledTime = (currentHour % 2 == 0 && currentMinute == 5);

  // Wenn es Zeit für eine Messung ist und seit der letzten Messung genug Zeit vergangen ist
  if (isScheduledTime && (millis() - lastPhMeasurement > 55000)) {  // Mind. 55 Sekunden seit letzter Messung
    // Hier verwenden wir direkt den laufend berechneten pH-Wert
    float phValue = getAveragePH();

    if (phValue >= 0 && phValue <= 14) {
      currentPH = phValue;

      // pH-Wert speichern
      savePHMeasurement(phValue);
      lastPhMeasurement = millis();

      Serial.print("Geplante pH-Messung durchgeführt: ");
      Serial.print(phValue, 2);
      Serial.print(" (Spannung: ");
      Serial.print(pHVoltage);
      Serial.print(" mV, bei konstanter Temperatur: ");
      Serial.print(FIXED_TEMPERATURE);
      Serial.println("°C)");
    } else {
      Serial.print("Fehler bei geplanter pH-Messung: Ungültiger Wert ");
      Serial.println(phValue);
    }
  }
}

// JSON für pH-Messungen generieren
// Doc muss alle pH-Einträge + Kalibrierdaten halten. Pro Eintrag ~112 Byte
// ArduinoJson-Overhead. Bei 12 Messungen/Tag (schedulePhMeasurement) und
// beliebiger Retention reichen 256 KB bis weit über 5 Jahre Daten.
void buildPHMeasurementsJson(PsramPrint& out) {
  PsramJsonDocument doc(262144);
  JsonArray phArray = doc.createNestedArray("ph");

  int phCount;
  PhMeasurement* phMeasurements = getAllPHMeasurements(phCount);

  for (int i = 0; i < phCount; i++) {
    JsonObject measurement = phArray.createNestedObject();
    measurement["timestamp"] = phMeasurements[i].timestamp;
    measurement["value"] = phMeasurements[i].value;
    measurement["index"] = phMeasurements[i].index;
    measurement["date"] = formatDateTime(phMeasurements[i].timestamp);
  }

  psram_delete_array(phMeasurements);

  // Aktuelle Messwerte und Kalibrierungsstatus
  float voltage = pHVoltage;
  float currentMeasurement = getPHFromVoltage(voltage);

  // Nur gültige Werte berücksichtigen
  if (currentMeasurement >= 0 && currentMeasurement <= 14) {
    currentPH = currentMeasurement;
  }

  doc["currentPH"] = currentPH;
  doc["rawVoltage"] = voltage;
  doc["rawADC"] = averageArray(pHArray, PH_ARRAY_LENGTH);
  doc["isCalibrating"] = isPhCalibrating;
  doc["isCalibrationStable"] = isPhCalibrationStable;
  doc["fixedTemperature"] = FIXED_TEMPERATURE;

  // In getPHMeasurementsJson
  // Kalibrierungsdaten hinzufügen
  doc["phCalibration"]["ph4Voltage"] = phCal.voltage_pH4;
  doc["phCalibration"]["ph7Voltage"] = phCal.voltage_pH7;
  doc["phCalibration"]["isCalibrated"] = phCal.isCalibrated;

  // Zeitstempel im Klartext und als time_t (mit Typprüfung)
  doc["phCalibration"]["timestamp_pH4"] = (long)phCal.timestamp_pH4;
  doc["phCalibration"]["timestamp_pH7"] = (long)phCal.timestamp_pH7;

  // Debug-Ausgabe zur Kontrolle
  Serial.print("JSON für Web: pH4 Zeitstempel = ");
  Serial.print(phCal.timestamp_pH4);
  Serial.print(" (");
  Serial.print(formatDateTime(phCal.timestamp_pH4));
  Serial.println(")");

  // Formatierte Zeitstempel für bessere Lesbarkeit
  if (phCal.timestamp_pH4 > 0) {
    doc["phCalibration"]["formattedTime_pH4"] = formatDateTime(phCal.timestamp_pH4);
  } else {
    doc["phCalibration"]["formattedTime_pH4"] = "Nie";
  }

  if (phCal.timestamp_pH7 > 0) {
    doc["phCalibration"]["formattedTime_pH7"] = formatDateTime(phCal.timestamp_pH7);
  } else {
    doc["phCalibration"]["formattedTime_pH7"] = "Nie";
  }

  // Direkt in PSRAM-Puffer serialisieren (kein String im Heap).
  serializeJson(doc, out);
}

// Hilfsfunktion für lesbare Typen
String getDosageTypeName(int dosageType) {
  switch (dosageType) {
    case DOSAGE_TYPE_CALCIUM: return "Calcium";
    case DOSAGE_TYPE_MAGNESIUM: return "Magnesium";
    case DOSAGE_TYPE_KH_DAY: return "KH-Tag";
    case DOSAGE_TYPE_KH_NIGHT: return "KH-Nacht";
    default: return "Unbekannt";
  }
}

// Hilfsfunktion um den Dateipfad für einen Dosierungstyp zu erhalten
const char* getDosageFilePath(int dosageType) {
  switch (dosageType) {
    case DOSAGE_TYPE_CALCIUM: return CA_DOSAGES_FILE;
    case DOSAGE_TYPE_MAGNESIUM: return MG_DOSAGES_FILE;
    case DOSAGE_TYPE_KH_DAY: return KH_DAY_DOSAGES_FILE;
    case DOSAGE_TYPE_KH_NIGHT: return KH_NIGHT_DOSAGES_FILE;
    default: return "";  // Fehlerfall
  }
}

// Hilfsfunktion um zu prüfen, ob ein Dosierungstyp KH ist
bool isKHDosageType(int dosageType) {
  return dosageType == DOSAGE_TYPE_KH_DAY || dosageType == DOSAGE_TYPE_KH_NIGHT;
}

// Hilfsfunktion um zu prüfen, ob ein Dosierungstyp Calcium/Magnesium ist
bool isCalciumDosageType(int dosageType) {
  return dosageType == DOSAGE_TYPE_CALCIUM || dosageType == DOSAGE_TYPE_MAGNESIUM;
}

// Speichert Messwert in entsprechender Datei
void saveMeasurement(float value, bool isCalcium) {
  // Skip if value is 0
  if (value <= 0) return;

  Serial.print("Originaler Messwert: ");
  Serial.println(value);

  // Neue Messung erstellen
  Measurement newMeasurement;
  newMeasurement.timestamp = getCurrentTime();
  newMeasurement.value = value;

  // Bestimme nächsten freien Index
  if (isCalcium) {
    newMeasurement.index = getNextMeasurementIndex(CA_MEASUREMENTS_FILE);
    File file = LittleFS.open(CA_MEASUREMENTS_FILE, FILE_APPEND);
    if (file) {
      file.write((uint8_t*)&newMeasurement, sizeof(Measurement));
      file.close();
      Serial.print("Calcium-Messwert gespeichert: ");
    }
  } else {
    newMeasurement.index = getNextMeasurementIndex(KH_MEASUREMENTS_FILE);
    File file = LittleFS.open(KH_MEASUREMENTS_FILE, FILE_APPEND);
    if (file) {
      file.write((uint8_t*)&newMeasurement, sizeof(Measurement));
      file.close();
      Serial.print("KH-Messwert gespeichert: ");
    }
  }

  // Alte Daten bereinigen (enthält yield() zwischen Dateien)
  cleanupOldData();

  yield();  // Watchdog Reset vor schwerer Berechnung

  // Nach dem Speichern neue Messung verarbeiten
  if (!blockCalculations) {
    // Neue Messung verarbeiten - aktualisiert nur den relevanten Plan
    handleNewMeasurement(isCalcium);
  } else {
    Serial.println("Pumpe aktiv, Dosierplan-Neuberechnung verzögert");
    pendingMeasurementRecalc = true;
    if (isCalcium) pendingRecalcIsCalcium = true;
    else pendingRecalcIsKH = true;
  }
}

// Automatische KH-Messung vom Tester speichern (separate Datei)
bool saveAutoKHMeasurement(float value, time_t timestamp = 0) {
  // Skip if value is 0
  if (value <= 0) {
    Serial.println("Auto-KH: Ungültiger Wert <= 0");
    return false;
  }

  Serial.printf("[Auto-KH] Empfangen: %.2f dKH, übergebener Timestamp: %ld\n", value, (long)timestamp);

  // Neue Messung erstellen
  Measurement newMeasurement;
  // Verwende übergebenen Timestamp oder aktuellen Zeitpunkt
  newMeasurement.timestamp = (timestamp > 0) ? (uint32_t)timestamp : (uint32_t)getCurrentTime();
  newMeasurement.value = value;
  newMeasurement.index = getNextMeasurementIndex(AUTO_KH_MEASUREMENTS_FILE);

  Serial.printf("[Auto-KH] Gespeicherter Timestamp: %u (%s), Index: %d\n",
                newMeasurement.timestamp, formatDateTime(newMeasurement.timestamp).c_str(),
                newMeasurement.index);

  // In separate Datei speichern
  File file = LittleFS.open(AUTO_KH_MEASUREMENTS_FILE, FILE_APPEND);
  if (!file) {
    Serial.println("[Auto-KH] FEHLER: Kann Auto-KH-Datei nicht öffnen");
    return false;
  }

  // Schreiben und Rückgabewert prüfen
  size_t bytesWritten = file.write((uint8_t*)&newMeasurement, sizeof(Measurement));
  file.flush();  // Buffer leeren vor close()
  file.close();

  if (bytesWritten != sizeof(Measurement)) {
    Serial.printf("[Auto-KH] FEHLER: Schreiben fehlgeschlagen (geschrieben: %d, erwartet: %d)\n",
                  bytesWritten, sizeof(Measurement));
    return false;
  }

  // Dateigröße prüfen → Anzahl gespeicherter Einträge
  File checkFile = LittleFS.open(AUTO_KH_MEASUREMENTS_FILE, FILE_READ);
  if (checkFile) {
    int totalEntries = checkFile.size() / sizeof(Measurement);
    checkFile.close();
    Serial.printf("[Auto-KH] ✓ Gespeichert: %.2f dKH, jetzt %d Einträge in Datei\n", value, totalEntries);
  }

  // Alte Daten bereinigen
  cleanupOldData();

  // WICHTIG: Automatische Messungen lösen KEINE Neuberechnung aus
  // Sie sind nur zur Referenz
  Serial.println("Auto-KH ist nur Referenz, keine Dosierplan-Neuberechnung");
  return true;
}

// Einzelne Dosierung speichern
void saveDosage(int pumpIndex, float amount, bool isAutomatic, float factor) {
  // Validating input parameters
  if (pumpIndex < 0 || pumpIndex >= 4) {
    Serial.print("Invalid pump index in saveDosage: ");
    Serial.println(pumpIndex);
    return;
  }

  if (amount <= 0) {
    Serial.print("Invalid amount in saveDosage: ");
    Serial.println(amount);
    return;
  }

  // Dosierungstyp bestimmen
  int dosageType;
  switch (pumpIndex) {
    case 0: dosageType = DOSAGE_TYPE_CALCIUM; break;
    case 1: dosageType = DOSAGE_TYPE_MAGNESIUM; break;
    case 2: dosageType = DOSAGE_TYPE_KH_DAY; break;
    case 3: dosageType = DOSAGE_TYPE_KH_NIGHT; break;
    default:
      Serial.print("Unknown dosage type for pump index: ");
      Serial.println(pumpIndex);
      return;
  }

  const char* filename = getDosageFilePath(dosageType);
  if (!filename || strlen(filename) == 0) {
    Serial.print("Invalid filename for dosage type: ");
    Serial.println(dosageType);
    return;
  }

  // Ensure the LittleFS is properly initialized
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS initialization failed in saveDosage!");
    return;
  }

  // Neue Dosierung erstellen - alle KH-Nacht-Dosierungen als physische ml speichern
  Dosage newDosage;
  newDosage.timestamp = getCurrentTime();
  newDosage.amount = amount;  // WICHTIG: Immer physische ml speichern
  newDosage.pumpIndex = pumpIndex;
  newDosage.dosageType = dosageType;
  newDosage.isAutomatic = isAutomatic;
  newDosage.factor = factor;
  newDosage.index = getNextDosageIndex(filename);

  // Speichern
  File file = LittleFS.open(filename, FILE_APPEND);
  if (!file) {
    Serial.print("Failed to open file for writing: ");
    Serial.println(filename);

    // Try to create the file if it doesn't exist
    file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
      Serial.print("Failed to create dosage file: ");
      Serial.println(filename);
      return;
    }
  }

  size_t bytesWritten = file.write((uint8_t*)&newDosage, sizeof(Dosage));
  if (bytesWritten != sizeof(Dosage)) {
    Serial.print("Failed to write dosage data. Wrote ");
    Serial.print(bytesWritten);
    Serial.print(" bytes, expected ");
    Serial.println(sizeof(Dosage));
  }

  file.close();

  if (bytesWritten == sizeof(Dosage)) {
    // RAM-Cache aktualisieren (vermeidet LittleFS-Lesen in checkAndPerformAutoDosing)
    if (dosageType >= 0 && dosageType < 4) {
      lastDosageTimeCache[dosageType] = newDosage.timestamp;
    }

    Serial.print(getDosageTypeName(dosageType));
    Serial.print("-Dosierung gespeichert: ");
    Serial.print(amount);  // WICHTIG: amount in der Ausgabe
    Serial.print("ml, Index: ");
    Serial.print(newDosage.index);
    Serial.print(", Timestamp: ");
    Serial.println(formatDateTime(newDosage.timestamp));
  }

  // Nach dem Speichern Verbrauch berechnen und Dosierplan aktualisieren
  if (!blockCalculations) {
    calculateConsumption();
    updateDosagePlans(true, true);
  } else {
    Serial.println("Pumpe aktiv, Dosierplan-Neuberechnung verzögert");
    pendingMeasurementRecalc = true;
    pendingRecalcIsKH = true;
    pendingRecalcIsCalcium = true;
  }
}

// Messwert löschen
void deleteMeasurement(int index, bool isCalcium) {
  const char* filename = isCalcium ? CA_MEASUREMENTS_FILE : KH_MEASUREMENTS_FILE;
  if (tombstoneByIndex<Measurement>(filename, index)) {
    Serial.print("Messwert mit Index ");
    Serial.print(index);
    Serial.println(" gelöscht (tombstone)");

    if (!blockCalculations) {
      TaskItem t; t.type = TASK_UPDATE_DOSAGE_PLANS; t.boolParam2 = true; t.boolParam3 = true;
      enqueueTask(t);
    }
  } else {
    Serial.print("Messwert mit Index ");
    Serial.print(index);
    Serial.println(" nicht gefunden");
  }
}

// Automatische KH-Messung löschen (Tombstone)
void deleteAutoKHMeasurement(int index) {
  if (tombstoneByIndex<Measurement>(AUTO_KH_MEASUREMENTS_FILE, index)) {
    Serial.print("Auto-KH-Messwert mit Index ");
    Serial.print(index);
    Serial.println(" gelöscht (tombstone)");
  } else {
    Serial.print("Auto-KH-Messwert mit Index ");
    Serial.print(index);
    Serial.println(" nicht gefunden");
  }
}

// Dosierung löschen
void deleteDosage(int index, int dosageType) {
  const char* filename = getDosageFilePath(dosageType);
  if (!filename || strlen(filename) == 0) return;

  if (tombstoneByIndex<Dosage>(filename, index)) {
    Serial.print("Dosierung mit Index ");
    Serial.print(index);
    Serial.print(" vom Typ ");
    Serial.print(getDosageTypeName(dosageType));
    Serial.println(" gelöscht (tombstone)");

    if (!blockCalculations) {
      TaskItem t; t.type = TASK_UPDATE_DOSAGE_PLANS; t.boolParam2 = true; t.boolParam3 = true;
      enqueueTask(t);
    }
  } else {
    Serial.print("Dosierung mit Index ");
    Serial.print(index);
    Serial.print(" vom Typ ");
    Serial.print(getDosageTypeName(dosageType));
    Serial.println(" nicht gefunden");
  }
}

// Alle Messungen eines bestimmten Typs (KH oder Calcium) holen
Measurement* getAllMeasurements(bool isCalcium, int& count) {
  const char* filename = isCalcium ? CA_MEASUREMENTS_FILE : KH_MEASUREMENTS_FILE;
  Measurement* m = readValidEntries<Measurement>(filename, count);
  // Bestandscode erwartet ein Nicht-null Array auch bei count==0.
  if (m == nullptr) return psram_new_array<Measurement>(0);
  return m;
}

// Alle automatischen KH-Messungen holen (separate Funktion)
// WICHTIG: Aufrufer muss psram_delete_array() auf Rückgabewert aufrufen (wenn nicht nullptr)
Measurement* getAllAutoKHMeasurements(int& count) {
  return readValidEntries<Measurement>(AUTO_KH_MEASUREMENTS_FILE, count);
}

// === Auto-KH Adopted-Tracking ===

// Auto-KH-Wert in reguläre KH-Messungen übernehmen
void adoptAutoKHMeasurement(int index) {
  // Auto-KH-Messung mit dem gegebenen Index finden
  int count;
  Measurement* autoKhMeasurements = getAllAutoKHMeasurements(count);

  if (autoKhMeasurements == nullptr || count == 0) {
    Serial.println("Keine Auto-KH-Messungen vorhanden");
    return;
  }

  float khValue = 0;
  bool found = false;
  for (int i = 0; i < count; i++) {
    if (autoKhMeasurements[i].index == index) {
      khValue = autoKhMeasurements[i].value;
      found = true;
      break;
    }
  }
  psram_delete_array(autoKhMeasurements);

  if (!found) {
    Serial.printf("Auto-KH Index %d nicht gefunden\n", index);
    return;
  }

  Serial.printf("Übernehme Auto-KH: %.2f dKH (Index %d) → reguläre KH-Messung\n", khValue, index);

  // In reguläre KH-Messungen speichern (Dosierplan-Neuberechnung)
  saveMeasurement(khValue, false);

  Serial.printf("Auto-KH Index %d übernommen\n", index);
}

// Hilfsfunktion um alle KH-Dosierungen zusammen zu bekommen
Dosage* getAllDosages(int dosageType, int& count) {
  const char* filename = getDosageFilePath(dosageType);
  count = 0;

  if (!filename || strlen(filename) == 0) {
    Serial.print("Invalid dosage filename for type: ");
    Serial.println(dosageType);
    return psram_new_array<Dosage>(0);
  }

  Dosage* d = readValidEntries<Dosage>(filename, count);
  if (d == nullptr) return psram_new_array<Dosage>(0);
  return d;
}

// Initialisiert den RAM-Cache für die letzten Dosierungszeitstempel pro Typ.
// Liest jeweils nur den letzten Eintrag jeder LittleFS-Datei (nicht alle!).
// Wird einmalig in setup() aufgerufen.
void initLastDosageTimeCache() {
  for (int type = 0; type < 4; type++) {
    lastDosageTimeCache[type] = 0;

    const char* filename = getDosageFilePath(type);
    if (!filename || strlen(filename) == 0) continue;

    Dosage lastDosage;
    if (readLatestEntry<Dosage>(filename, lastDosage)) {
      lastDosageTimeCache[type] = lastDosage.timestamp;
      Serial.printf("Cache init: %s letzter Zeitstempel %ld\n",
                    getDosageTypeName(type), (long)lastDosage.timestamp);
    }
  }
  Serial.println("lastDosageTimeCache initialisiert");
}

// Stream-optimierte KH-Dosierungen (keine Triple-Allocation!)
Dosage* getAllKHDosages(int& count) {
  // Zähle Einträge direkt aus Dateien
  int khDayCount = 0;
  int khNightCount = 0;

  File khDayFile = LittleFS.open(KH_DAY_DOSAGES_FILE, FILE_READ);
  if (khDayFile) {
    khDayCount = khDayFile.size() / sizeof(Dosage);
    khDayFile.close();
  }

  File khNightFile = LittleFS.open(KH_NIGHT_DOSAGES_FILE, FILE_READ);
  if (khNightFile) {
    khNightCount = khNightFile.size() / sizeof(Dosage);
    khNightFile.close();
  }

  count = khDayCount + khNightCount;

  if (count == 0) {
    return psram_new_array<Dosage>(0);
  }

  // Allokiere nur einmal das finale Array
  Dosage* allDosages = psram_new_array<Dosage>(count);

  // Stream KH-Tag direkt ins Array (Index 0)
  khDayFile = LittleFS.open(KH_DAY_DOSAGES_FILE, FILE_READ);
  if (khDayFile && khDayCount > 0) {
    khDayFile.read((uint8_t*)allDosages, khDayCount * sizeof(Dosage));
    khDayFile.close();
  }

  // Stream KH-Nacht direkt ins Array (Index khDayCount)
  khNightFile = LittleFS.open(KH_NIGHT_DOSAGES_FILE, FILE_READ);
  if (khNightFile && khNightCount > 0) {
    khNightFile.read((uint8_t*)(allDosages + khDayCount), khNightCount * sizeof(Dosage));
    khNightFile.close();
  }

  return allDosages;
}

// Stream-optimierte Calcium+Magnesium-Dosierungen (keine Triple-Allocation!)
// Hinweis: Name ist historisch - kombiniert Ca UND Mg!
Dosage* getAllCalciumDosages(int& count) {
  // Zähle Einträge direkt aus Dateien
  int caCount = 0;
  int mgCount = 0;

  File caFile = LittleFS.open(CA_DOSAGES_FILE, FILE_READ);
  if (caFile) {
    caCount = caFile.size() / sizeof(Dosage);
    caFile.close();
  }

  File mgFile = LittleFS.open(MG_DOSAGES_FILE, FILE_READ);
  if (mgFile) {
    mgCount = mgFile.size() / sizeof(Dosage);
    mgFile.close();
  }

  count = caCount + mgCount;

  if (count == 0) {
    return psram_new_array<Dosage>(0);
  }

  // Allokiere nur einmal das finale Array
  Dosage* allDosages = psram_new_array<Dosage>(count);

  // Stream Calcium direkt ins Array (Index 0)
  caFile = LittleFS.open(CA_DOSAGES_FILE, FILE_READ);
  if (caFile && caCount > 0) {
    caFile.read((uint8_t*)allDosages, caCount * sizeof(Dosage));
    caFile.close();
  }

  // Stream Magnesium direkt ins Array (Index caCount)
  mgFile = LittleFS.open(MG_DOSAGES_FILE, FILE_READ);
  if (mgFile && mgCount > 0) {
    mgFile.read((uint8_t*)(allDosages + caCount), mgCount * sizeof(Dosage));
    mgFile.close();
  }

  return allDosages;
}

// Neuesten Messwert für einen bestimmten Typ holen
float getLatestValue(bool isCalcium) {
  const char* filename = isCalcium ? CA_MEASUREMENTS_FILE : KH_MEASUREMENTS_FILE;
  Measurement latest;
  if (readLatestEntry<Measurement>(filename, latest)) return latest.value;
  return 0;
}

// Zeitstempel der neuesten Messung für einen bestimmten Typ holen
time_t getLastMeasurementTimestamp(bool isCalcium) {
  const char* filename = isCalcium ? CA_MEASUREMENTS_FILE : KH_MEASUREMENTS_FILE;
  Measurement latest;
  if (readLatestEntry<Measurement>(filename, latest)) return latest.timestamp;
  return 0;
}

// =========== SPEICHER-MONITORING FUNKTIONEN ===========

// Neue Funktion für Speicherstatus mit Farblogik
void sendMemoryStatus() {
  DynamicJsonDocument doc(1024);

  // LittleFS Info
  size_t totalFS = LittleFS.totalBytes();
  size_t usedFS = LittleFS.usedBytes();
  size_t freeFS = totalFS - usedFS;
  float fsPercentFree = (float)freeFS / totalFS * 100;

  // Heap Info
  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  size_t maxAllocHeap = ESP.getMaxAllocHeap();
  float heapPercentFree = (float)freeHeap / totalHeap * 100;

  // Flash: Freier Platz in der App-Partition
  size_t sketchSize = ESP.getSketchSize();
  size_t freeSketch = ESP.getFreeSketchSpace();
  size_t partitionSize = sketchSize + freeSketch;
  float flashPercentFree = (partitionSize > 0) ? (float)freeSketch / partitionSize * 100 : 0;

  // PSRAM Info
  size_t totalPsram = ESP.getPsramSize();
  size_t freePsram = ESP.getFreePsram();
  float psramPercentFree = (totalPsram > 0) ? (float)freePsram / totalPsram * 100 : 0;

  // Status bestimmen (worst case von allen, PSRAM nur wenn vorhanden)
  String overallStatus = "good";
  float minPercent = min({fsPercentFree, heapPercentFree, flashPercentFree});
  if (totalPsram > 0) minPercent = min(minPercent, psramPercentFree);
  if (minPercent < 20) {
    overallStatus = "critical";
  } else if (minPercent < 50) {
    overallStatus = "warning";
  }

  doc["type"] = "memoryStatus";
  doc["timestamp"] = getCurrentTime();

  // LittleFS
  JsonObject littlefs = doc.createNestedObject("littlefs");
  littlefs["total"] = totalFS;
  littlefs["used"] = usedFS;
  littlefs["free"] = freeFS;
  littlefs["percentFree"] = fsPercentFree;

  // Heap
  JsonObject heap = doc.createNestedObject("heap");
  heap["free"] = freeHeap;
  heap["total"] = totalHeap;
  heap["maxAlloc"] = maxAllocHeap;
  heap["percentFree"] = heapPercentFree;

  // Flash (App-Partition)
  JsonObject flash = doc.createNestedObject("flash");
  flash["total"] = partitionSize;
  flash["sketch"] = sketchSize;
  flash["free"] = freeSketch;
  flash["percentFree"] = flashPercentFree;

  // PSRAM (nur wenn vorhanden)
  if (totalPsram > 0) {
    JsonObject psram = doc.createNestedObject("psram");
    psram["total"] = totalPsram;
    psram["free"] = freePsram;
    psram["percentFree"] = psramPercentFree;
  }

  // Overall status
  doc["status"] = overallStatus;

  String jsonString;
  serializeJson(doc, jsonString);
  // Nur senden wenn Clients verbunden sind
  if (ws.count() > 0) {
    ws.textAll(jsonString);
  }
}

// =========== SCHWANKUNGSKOMPENSATION ===========

// KH-Muster erkennen: Mindestens 6 Messungen über ≥20h mit max 4h Abstand
// Sucht nur das neueste Muster. Gibt true zurück wenn ein (neues) Muster gefunden wurde.
bool detectKHPattern() {
  int count = 0;
  Measurement* measurements = getAllAutoKHMeasurements(count);
  if (!measurements || count < 6) {
    if (measurements) psram_delete_array(measurements);
    hasNewPattern = false;
    return false;
  }

  // Nach Zeitstempel sortieren (neueste zuerst)
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (measurements[j].timestamp > measurements[i].timestamp) {
        Measurement tmp = measurements[i];
        measurements[i] = measurements[j];
        measurements[j] = tmp;
      }
    }
  }

  // Vom neuesten Eintrag rückwärts gruppieren (max 4h Abstand)
  int groupStart = 0;  // neuester (Index 0 = neuester nach Sortierung)
  int groupEnd = 0;

  for (int i = 1; i < count; i++) {
    uint32_t gap = measurements[i - 1].timestamp - measurements[i].timestamp;
    if (gap > 4 * 3600) {
      break;  // Lücke > 4h → Gruppe beendet
    }
    groupEnd = i;
  }

  int groupSize = groupEnd - groupStart + 1;
  uint32_t span = measurements[groupStart].timestamp - measurements[groupEnd].timestamp;
  float spanHours = span / 3600.0;

  Serial.printf("KH-Muster: %d Messungen, %.1f Stunden Spanne\n", groupSize, spanHours);

  if (groupSize >= 6 && spanHours >= 20.0) {
    // Muster gefunden - ist es neuer als das letzte genutzte?
    time_t patternEnd = measurements[groupStart].timestamp;
    if (patternEnd > lastUsedPatternEnd) {
      hasNewPattern = true;
      Serial.println("Neues KH-Schwankungsmuster erkannt!");
    } else {
      hasNewPattern = false;
    }
    psram_delete_array(measurements);
    return hasNewPattern;
  }

  hasNewPattern = false;
  psram_delete_array(measurements);
  return false;
}

// Hilfsfunktion: Berechnet Dosier-Faktoren aus KH-Änderungsraten
// rates[12] = KH-Änderungsrate pro 2h-Bucket, outFactors[12] = berechnete Faktoren
void computeFactorsFromRates(float rates[12], float outFactors[12]) {
  float avgRate = 0;
  for (int i = 0; i < 12; i++) avgRate += rates[i];
  avgRate /= 12.0;

  float maxDeviation = 0;
  for (int i = 0; i < 12; i++) {
    float dev = fabs(rates[i] - avgRate);
    if (dev > maxDeviation) maxDeviation = dev;
  }

  if (maxDeviation < 0.001) {
    for (int i = 0; i < 12; i++) outFactors[i] = 1.0;
    return;
  }

  for (int i = 0; i < 12; i++) {
    outFactors[i] = 1.0 + (avgRate - rates[i]) / maxDeviation * 0.5;
    outFactors[i] = max(0.3f, min(2.0f, outFactors[i]));
  }

  // Normalisieren auf Durchschnitt 1.0
  float sum = 0;
  for (int i = 0; i < 12; i++) sum += outFactors[i];
  float normFactor = 12.0 / sum;
  for (int i = 0; i < 12; i++) {
    outFactors[i] *= normFactor;
    outFactors[i] = max(0.3f, min(2.0f, outFactors[i]));
  }
}

// Schwankungskompensation-Faktoren berechnen
// Analysiert das neueste KH-Tagesmuster und erstellt 12 Intervall-Faktoren
void calculateDosingFactorsFromPattern() {
  int count = 0;
  Measurement* measurements = getAllAutoKHMeasurements(count);
  if (!measurements || count < 6) {
    if (measurements) psram_delete_array(measurements);
    Serial.println("Nicht genug Messungen für Faktorberechnung");
    return;
  }

  // Nach Zeitstempel sortieren (neueste zuerst)
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (measurements[j].timestamp > measurements[i].timestamp) {
        Measurement tmp = measurements[i];
        measurements[i] = measurements[j];
        measurements[j] = tmp;
      }
    }
  }

  // Neuestes Muster finden (gleiche Logik wie detectKHPattern)
  int groupStart = 0;
  int groupEnd = 0;
  for (int i = 1; i < count; i++) {
    uint32_t gap = measurements[i - 1].timestamp - measurements[i].timestamp;
    if (gap > 4 * 3600) break;
    groupEnd = i;
  }

  int groupSize = groupEnd - groupStart + 1;
  uint32_t span = measurements[groupStart].timestamp - measurements[groupEnd].timestamp;

  if (groupSize < 6 || span < 20 * 3600) {
    psram_delete_array(measurements);
    Serial.println("Kein gültiges Muster für Faktorberechnung");
    return;
  }

  // Muster in chronologischer Reihenfolge verarbeiten (älteste zuerst)
  // groupEnd = ältester, groupStart = neuester
  float freshRates[12] = {0};
  float rateWeights[12] = {0};  // Gewichtung pro Bucket (Stunden-Abdeckung)

  // Lambda: Ein Messwertpaar auf 2h-Buckets verteilen
  auto distributePair = [&](float khOlder, time_t tsOlder, float khNewer, time_t tsNewer) {
    float deltaHours = (float)(tsNewer - tsOlder) / 3600.0;
    if (deltaHours <= 0) return;

    float ratePerHour = (khNewer - khOlder) / deltaHours;  // dKH/h (negativ = Verbrauch)
    int startHour = Europe_Berlin.hour(tsOlder);

    // Rate auf 2h-Buckets verteilen
    float remainingHours = deltaHours;
    int currentH = startHour;

    while (remainingHours > 0.01) {
      int bucket = currentH / 2;  // 0-11
      if (bucket < 0) bucket = 0;
      if (bucket > 11) bucket = 11;

      int bucketEnd = (bucket + 1) * 2;
      float hoursInBucket = min(remainingHours, (float)(bucketEnd - currentH));
      if (hoursInBucket <= 0) hoursInBucket = min(remainingHours, 2.0f);

      freshRates[bucket] += ratePerHour * hoursInBucket;
      rateWeights[bucket] += hoursInBucket;

      remainingHours -= hoursInBucket;
      currentH = bucketEnd % 24;
    }
  };

  // Alle aufeinanderfolgenden Paare verarbeiten (chronologisch)
  for (int i = groupEnd; i > groupStart; i--) {
    distributePair(measurements[i].value, measurements[i].timestamp,
                   measurements[i - 1].value, measurements[i - 1].timestamp);
  }

  // Wrap-around: Neueste Messung → Älteste Messung + 24h (zyklisches Tagesmuster)
  // Verbindet z.B. 22:59 mit 02:00 am nächsten Tag
  {
    time_t oldestTs = measurements[groupEnd].timestamp;
    time_t newestTs = measurements[groupStart].timestamp;
    float wrapHours = (float)(oldestTs + 24 * 3600 - newestTs) / 3600.0;
    // Nur wenn die Lücke realistisch ist (< 8h, sonst ist es kein sinnvoller Wrap)
    if (wrapHours > 0 && wrapHours < 8.0) {
      Serial.printf("Wrap-around: %.1fh Lücke, KH %.1f → %.1f\n", wrapHours,
                     measurements[groupStart].value, measurements[groupEnd].value);
      distributePair(measurements[groupStart].value, newestTs,
                     measurements[groupEnd].value, oldestTs + 24 * 3600);
    }
  }

  // Gewichtete Durchschnittsrate pro Bucket
  float avgRate = 0;
  int activeBuckets = 0;
  for (int i = 0; i < 12; i++) {
    if (rateWeights[i] > 0) {
      freshRates[i] /= rateWeights[i];  // Durchschnittliche Rate pro Stunde
      avgRate += freshRates[i];
      activeBuckets++;
    }
  }

  if (activeBuckets == 0) {
    psram_delete_array(measurements);
    Serial.println("Keine gültigen Bucket-Daten");
    return;
  }
  avgRate /= activeBuckets;

  // Buckets ohne Daten mit Durchschnitt füllen
  for (int i = 0; i < 12; i++) {
    if (rateWeights[i] == 0) {
      freshRates[i] = avgRate;
    }
  }

  // Neue Faktoren aus den frischen Raten berechnen
  float newFactors[12];
  computeFactorsFromRates(freshRates, newFactors);

  // Prüfe ob altes Muster vorhanden
  bool hasOldPattern = false;
  for (int i = 0; i < 12; i++) {
    if (patternChangeRates[i] != 0) {
      hasOldPattern = true;
      break;
    }
  }

  if (hasOldPattern && dosingFactorsEnabled) {
    // Kompensation war AKTIV während der neuen Messung
    // → Neue Faktoren sind Restkorrekturen auf die bestehende Kompensation
    // → Multiplikation: alter Faktor × neuer Faktor
    Serial.println("Kompensation aktiv: Multipliziere alte Faktoren mit neuer Korrektur");
    for (int i = 0; i < 12; i++) {
      dosingFactors[i] = dosingFactors[i] * newFactors[i];
      dosingFactors[i] = max(0.3f, min(2.0f, dosingFactors[i]));
    }
    // Normalisieren auf Durchschnitt 1.0
    float sum = 0;
    for (int i = 0; i < 12; i++) sum += dosingFactors[i];
    float normFactor = 12.0 / sum;
    for (int i = 0; i < 12; i++) {
      dosingFactors[i] *= normFactor;
      dosingFactors[i] = max(0.3f, min(2.0f, dosingFactors[i]));
    }
  } else {
    // Kompensation war INAKTIV oder kein altes Muster
    // → Neue Faktoren direkt übernehmen (keine alten Faktoren zum Kombinieren)
    Serial.println("Neue Faktoren aus unkompensierter Messreihe");
    for (int i = 0; i < 12; i++) {
      dosingFactors[i] = newFactors[i];
    }
  }

  // Raten speichern für nächste Kombination
  for (int i = 0; i < 12; i++) {
    patternChangeRates[i] = freshRates[i];
  }

  // Metadaten aktualisieren
  lastUsedPatternEnd = measurements[groupStart].timestamp;
  lastFactorCalculation = getCurrentTime();
  hasNewPattern = false;

  // Speichern
  saveDosingFactors();

  // Debug-Ausgabe
  Serial.println("=== Schwankungskompensation-Faktoren berechnet ===");
  for (int i = 0; i < 12; i++) {
    Serial.printf("  %02d:00-%02d:00: Faktor %.3f (Rate: %.4f dKH/h)\n",
                  i * 2, (i + 1) * 2, dosingFactors[i], patternChangeRates[i]);
  }
  Serial.printf("Durchschnittsrate: %.4f dKH/h\n", avgRate);
  Serial.println("=================================================");

  psram_delete_array(measurements);
}

// Alte Daten bereinigen - alle Dateien nach DATA_RETENTION_DAYS entfernen
// yield() zwischen den Dateien um Watchdog-Timeouts zu vermeiden
void cleanupOldData() {
  time_t now = getCurrentTime();
  time_t cutoffTime = now - (DATA_RETENTION_DAYS * 24 * 60 * 60);

  // Bereinige alle Dateien mit der gleichen Retention-Regel
  // yield() gibt dem Watchdog und anderen Tasks (WiFi, TCP) Zeit
  cleanupFile<Measurement>(KH_MEASUREMENTS_FILE, cutoffTime);
  yield();
  cleanupFile<Measurement>(CA_MEASUREMENTS_FILE, cutoffTime);
  yield();
  cleanupFile<Measurement>(AUTO_KH_MEASUREMENTS_FILE, cutoffTime);
  yield();
  cleanupFile<Dosage>(CA_DOSAGES_FILE, cutoffTime);
  yield();
  cleanupFile<Dosage>(MG_DOSAGES_FILE, cutoffTime);
  yield();
  cleanupFile<Dosage>(KH_DAY_DOSAGES_FILE, cutoffTime);
  yield();
  cleanupFile<Dosage>(KH_NIGHT_DOSAGES_FILE, cutoffTime);
  yield();
  cleanupFile<PhMeasurement>(PH_MEASUREMENTS_FILE, cutoffTime);

  Serial.printf("Cleanup: Daten aelter als %d Tage bereinigt\n", DATA_RETENTION_DAYS);
}

// ==================== EVENT-LOGGER FÜR DOSIERUNGS-DEBUGGING ====================

// Schreibt ein Dosierungs-Event in die Log-Datei
void logDosingEvent(const String& event, const String& details) {
  if (!timeInitialized) return;  // Nur loggen wenn Zeit verfügbar

  File logFile = LittleFS.open(DOSING_EVENT_LOG, FILE_APPEND);
  if (!logFile) {
    Serial.println("Fehler beim Öffnen der Event-Log-Datei");
    return;
  }

  // Format: [Timestamp] [Event] [Details]
  time_t now = getCurrentTime();
  String logEntry = formatDateTime(now) + " | " + event;
  if (details.length() > 0) {
    logEntry += " | " + details;
  }
  logEntry += "\n";

  logFile.print(logEntry);
  logFile.close();

  // Prüfe Log-Größe und bereinige wenn nötig
  File checkFile = LittleFS.open(DOSING_EVENT_LOG, FILE_READ);
  if (checkFile && checkFile.size() > MAX_LOG_SIZE) {
    checkFile.close();
    cleanupDosingEventLog();
  } else if (checkFile) {
    checkFile.close();
  }
}

// Bereinigt alte Einträge aus dem Event-Log (älter als 2 Tage)
void cleanupDosingEventLog() {
  if (!LittleFS.exists(DOSING_EVENT_LOG)) return;

  File sourceFile = LittleFS.open(DOSING_EVENT_LOG, FILE_READ);
  if (!sourceFile) {
    Serial.println("Fehler beim Öffnen der Event-Log-Datei für Bereinigung");
    return;
  }

  // Temporäre Datei erstellen
  const char* tempFile = "/dosing_events_temp.log";
  File destFile = LittleFS.open(tempFile, FILE_WRITE);
  if (!destFile) {
    Serial.println("Fehler beim Erstellen der temporären Log-Datei");
    sourceFile.close();
    return;
  }

  time_t now = getCurrentTime();
  time_t cutoffTime = now - LOG_RETENTION_TIME;
  int keptLines = 0;
  int removedLines = 0;

  // Lese Zeile für Zeile
  while (sourceFile.available()) {
    String line = sourceFile.readStringUntil('\n');

    // Extrahiere Datum aus dem Format "DD.MM.YY HH:MM:SS | ..."
    if (line.length() > 17) {
      String dateStr = line.substring(0, 17);  // "DD.MM.YY HH:MM:SS"

      // Parse Datum (vereinfacht - nimmt an, dass Format korrekt ist)
      int day = dateStr.substring(0, 2).toInt();
      int month = dateStr.substring(3, 5).toInt();
      int year = 2000 + dateStr.substring(6, 8).toInt();
      int hour = dateStr.substring(9, 11).toInt();
      int minute = dateStr.substring(12, 14).toInt();
      int second = dateStr.substring(15, 17).toInt();

      tmElements_t tm;
      tm.Year = year - 1970;
      tm.Month = month;
      tm.Day = day;
      tm.Hour = hour;
      tm.Minute = minute;
      tm.Second = second;

      time_t lineTime = makeTime(tm);

      // Nur Zeilen behalten die neuer als cutoff sind
      if (lineTime >= cutoffTime) {
        destFile.println(line);
        keptLines++;
      } else {
        removedLines++;
      }
    } else {
      // Ungültige Zeile, trotzdem behalten
      destFile.println(line);
      keptLines++;
    }
  }

  sourceFile.close();
  destFile.close();

  // Ersetze alte Datei durch neue
  LittleFS.remove(DOSING_EVENT_LOG);
  LittleFS.rename(tempFile, DOSING_EVENT_LOG);

  Serial.print("Event-Log bereinigt: ");
  Serial.print(keptLines);
  Serial.print(" behalten, ");
  Serial.print(removedLines);
  Serial.println(" entfernt");
}

// Liest die letzten N Zeilen aus dem Event-Log
String getDosingEventLog(int maxLines) {
  if (!LittleFS.exists(DOSING_EVENT_LOG)) {
    return "Keine Event-Logs vorhanden";
  }

  File logFile = LittleFS.open(DOSING_EVENT_LOG, FILE_READ);
  if (!logFile) {
    return "Fehler beim Lesen der Event-Log-Datei";
  }

  // Zähle Gesamtanzahl Zeilen
  int totalLines = 0;
  while (logFile.available()) {
    logFile.readStringUntil('\n');
    totalLines++;
  }

  // Zurück zum Anfang
  logFile.seek(0);

  // Überspringe alte Zeilen
  int linesToSkip = (totalLines > maxLines) ? (totalLines - maxLines) : 0;
  for (int i = 0; i < linesToSkip; i++) {
    logFile.readStringUntil('\n');
  }

  // Lese die letzten maxLines Zeilen
  String result = "";
  while (logFile.available()) {
    result += logFile.readStringUntil('\n');
    result += "\n";
  }

  logFile.close();
  return result;
}

// Funktion zum Prüfen, ob nur eine Messung für KH oder Calcium vorliegt
bool isFirstMeasurement(bool isCalcium) {
  int count;
  Measurement* measurements = getAllMeasurements(isCalcium, count);
  psram_delete_array(measurements);
  return count == 1;
}

// Sortiert aufsteigend nach timestamp. Da alle Leser (readValidEntries) bereits
// in Append-Reihenfolge liefern (= aufsteigend), ist das ein No-Op im
// Normalfall. Schutz nur gegen degenerierte Auto-KH-Inserts mit rückwärts-
// datiertem Timestamp: Insertion-Sort mit Early-Exit, O(n) bei sortierten Daten.
void sortMeasurementsByTimestamp(Measurement* measurements, int count) {
  for (int i = 1; i < count; i++) {
    Measurement key = measurements[i];
    int j = i - 1;
    while (j >= 0 && measurements[j].timestamp > key.timestamp) {
      measurements[j + 1] = measurements[j];
      j--;
    }
    measurements[j + 1] = key;
  }
}

// Sortiert absteigend (neueste zuerst). Input ist aufsteigend (Append-Only),
// also reicht In-Place-Reverse in O(n) statt O(n²).
void sortDosagesByTimestamp(Dosage* dosages, int count) {
  for (int a = 0, b = count - 1; a < b; a++, b--) {
    Dosage tmp = dosages[a];
    dosages[a] = dosages[b];
    dosages[b] = tmp;
  }
  if (count > 0) {
    Serial.print("Nach Sortierung: Neuester Eintrag ist von ");
    Serial.println(formatDateTime(dosages[0].timestamp));
  }
}
// =========== PUMPEN-FUNKTIONEN ===========

// Dosierungsauftrag zur Warteschlange hinzufügen
bool addToDosingQueue(int pumpIndex, float amount, bool isAutomatic, float factor) {
  // Überprüfe, ob die Warteschlange voll ist
  int nextTail = (queueTail + 1) % MAX_DOSING_QUEUE;
  if (!queueEmpty && nextTail == queueHead) {
    Serial.println("Fehler: Dosierungswarteschlange ist voll!");
    return false;
  }

  // Neuen Auftrag hinzufügen
  dosingQueue[queueTail].pumpIndex = pumpIndex;
  dosingQueue[queueTail].amount = amount;
  dosingQueue[queueTail].isAutomatic = isAutomatic;
  dosingQueue[queueTail].factor = factor;

  Serial.print("Dosierungsauftrag zur Warteschlange hinzugefügt: ");
  Serial.print(amount);
  Serial.print(" ml mit Pumpe ");
  Serial.print(pumpIndex);
  Serial.print(" (");
  Serial.print(pumps[pumpIndex].name);
  Serial.println(")");

  // Warteschlange aktualisieren
  queueTail = nextTail;
  queueEmpty = false;

  // Starte die Warteschlange, wenn noch keine Dosierung aktiv ist
  if (activePumpIndex == -1) {
    startNextDosingJob();
  }

  return true;
}

// Diese angepasste Funktion berücksichtigt die Anti-Tropf-Funktion bei der Berechnung der Dosierschritte
void startNextDosingJob() {
  logDosingEvent("START_NEXT_CALL", "queueEmpty=" + String(queueEmpty ? "true" : "false") + " activePump=" + String(activePumpIndex));

  if (queueEmpty) {
    logDosingEvent("START_NEXT_EMPTY", "Keine Aufträge in Queue");
    return;  // Keine weiteren Aufträge
  }

  // Sicherheitscheck: Keine neue Dosierung starten, wenn bereits eine Pumpe aktiv ist
  if (activePumpIndex != -1) {
    logDosingEvent("START_NEXT_BLOCKED", "Pumpe bereits aktiv: " + String(activePumpIndex));
    Serial.println("Kann nächste Dosierung nicht starten: Eine Pumpe ist bereits aktiv");
    return;
  }

  // Nächsten Auftrag holen
  DosingJob job = dosingQueue[queueHead];

  // Warteschlange aktualisieren
  queueHead = (queueHead + 1) % MAX_DOSING_QUEUE;
  queueEmpty = (queueHead == queueTail);

  Serial.print("Warteschlange nach Update: Head=");
  Serial.print(queueHead);
  Serial.print(", Tail=");
  Serial.print(queueTail);
  Serial.print(", Leer=");
  Serial.println(queueEmpty ? "Ja" : "Nein");

  logDosingEvent("START_NEXT_JOB", "Pumpe=" + String(job.pumpIndex) + " ml=" + String(job.amount, 2) + " auto=" + String(job.isAutomatic ? "true" : "false"));
  Serial.print("Starte nächsten Dosierungsauftrag: ");
  Serial.print(job.amount);
  Serial.print(" ml mit Pumpe ");
  Serial.print(job.pumpIndex);
  Serial.print(" (");
  Serial.print(pumps[job.pumpIndex].name);
  Serial.println(")");

  // Prüfe, ob die Pumpe kalibriert ist
  if (pumps[job.pumpIndex].mlPerStep <= 0) {
    logDosingEvent("PUMP_NOT_CAL", "Pumpe " + String(job.pumpIndex) + " nicht kalibriert");
    Serial.println("Fehler: Pumpe nicht kalibriert!");

    // Wenn Teil einer Sequenz, reduziere den Zähler
    if (job.isAutomatic && dosageSequenceActive) {
      pendingDosages--;
      Serial.print("Dosierung übersprungen, verbleibende Dosierungen: ");
      Serial.println(pendingDosages);

      // Wenn alle geplanten Dosierungen abgeschlossen sind
      if (pendingDosages <= 0) {
        time_t now = getCurrentTime();
        settings.lastAutoDosage = currentDosageTimestamp;
        saveSettingsToJson();

        Serial.print("Dosierungssequenz abgeschlossen für Stunde: ");
        Serial.print(currentDosageHour);
        Serial.print(" um ");
        Serial.println(formatDateTime(now));

        dosageSequenceActive = false;
      }
    }

    // Statt rekursivem Aufruf setzen wir den Flag
    needToStartNextJob = true;
    return;
  }

  // Dosierungswerte speichern
  currentDosageAmount = job.amount;
  activePumpIndex = job.pumpIndex;
  currentDosageFactor = job.factor;
  currentDosingIsAutomatic = job.isAutomatic;

  // Alte Zustandsvariablen für Kompatibilität aktualisieren
  isDispensingRunning = true;

  // Status setzen basierend auf Dosierungstyp
  pumpState = job.isAutomatic ? PUMP_AUTO_DISPENSING : PUMP_MANUAL_DISPENSING;

  // Info für spätere Referenz speichern
  lastPumpOperation = job.pumpIndex;
  lastPumpAmount = job.amount;

  // Wenn Anti-Tropf aktiviert ist, erst mit Vorschub beginnen
  if (settings.enableAntiDrip) {
    startAntiDripPrime();
  } else {
    // Ohne Anti-Tropf direkt mit Dosierung beginnen
    startDosing();
  }
}

// Pumpe kalibrieren - ESP32 Version mit höherer Zeitpräzision
bool calibratePump(int pumpIndex, int steps) {
  if (pumpIndex < 0 || pumpIndex >= 4) {
    Serial.print("Fehler: Ungültiger Pumpenindex für Kalibrierung: ");
    Serial.println(pumpIndex);
    return false;
  }

  if (activePumpIndex != -1) {
    Serial.println("Fehler: Eine andere Pumpe ist bereits aktiv. Bitte warten Sie, bis der aktuelle Vorgang abgeschlossen ist.");
    return false;  // Eine andere Pumpe ist aktiv
  }

  if (steps <= 0) {
    Serial.println("Fehler: Ungültige Schrittanzahl für Kalibrierung");
    return false;
  }

  currentCalibrationPump = pumpIndex;
  currentCalibrationSteps = steps;
  targetSteps = steps;
  activePumpIndex = pumpIndex;
  blockCalculations = true;  // Blockiere Berechnungen während Kalibrierung

  // Alte Zustandsvariablen für Kompatibilität aktualisieren
  isCalibrationRunning = true;

  // Status setzen auf Kalibrierung
  pumpState = PUMP_CALIBRATING;

  // Richtung einstellen (vorwärts)
  digitalWrite(DIR_PIN, HIGH);

  // Nur die gewünschte Pumpe aktivieren
  activatePump(pumpIndex);

  // FastAccelStepper konfigurieren
  if (pumpenStepper[pumpIndex]) {
    pumpenStepper[pumpIndex]->setSpeedInHz(mlPerMinToStepsPerSec(pumps[pumpIndex].speedML, pumpIndex));

    if (pumps[pumpIndex].accelerationML > 0) {
      pumpenStepper[pumpIndex]->setAcceleration(mlPerMin2ToStepsPerSec2(pumps[pumpIndex].accelerationML, pumpIndex));
    } else {
      // Keine Beschleunigung
      pumpenStepper[pumpIndex]->setAcceleration(10000);  // Sehr hohe Beschleunigung = quasi sofort
    }

    // Positionierung auf Null und Bewegung starten
    pumpenStepper[pumpIndex]->setCurrentPosition(0);
    pumpenStepper[pumpIndex]->moveTo(steps);
  }

  Serial.print("Kalibrierung gestartet: Pumpe ");
  Serial.print(pumpIndex);
  Serial.print(" (");
  Serial.print(pumps[pumpIndex].name);
  Serial.print(") mit ");
  Serial.print(steps);
  Serial.println(" Schritten");

  return true;
}

// Globale Einstellungen für alle Pumpen setzen
void setGlobalSettings(float speedML, float accelerationML) {
  // Fallback-Werte beibehalten, falls ungültige Werte übergeben werden
  if (speedML < 0) speedML = DEFAULT_SPEED_ML;
  if (accelerationML < 0) accelerationML = DEFAULT_ACCELERATION_ML;

  // Für alle Pumpen die gleichen Werte setzen
  for (int i = 0; i < 4; i++) {
    pumps[i].speedML = speedML;
    pumps[i].accelerationML = accelerationML;
  }

  // In JSON speichern
  savePumpsToJson();

  Serial.println("Globale Einstellungen gespeichert:");
  Serial.print(" - ");
  Serial.print(speedML, 4);
  Serial.println(" ml/min");
  Serial.print(" - ");
  Serial.print(accelerationML, 4);
  Serial.println(" ml/min²");
}

// Flüssigkeit dosieren
void dispensePump(int pumpIndex, float ml, bool isAutomatic) {
  if (pumpIndex < 0 || pumpIndex >= 4 || ml <= 0) return;

  // Wenn bereits eine Pumpe aktiv ist, zur Warteschlange hinzufügen
  if (activePumpIndex != -1) {
    Serial.print("Pumpe ");
    Serial.print(activePumpIndex);
    Serial.println(" bereits aktiv, füge Auftrag zur Warteschlange hinzu");

    logDosingEvent("QUEUE_ADD", "Pumpe=" + String(pumpIndex) + " ml=" + String(ml, 2) + " activePump=" + String(activePumpIndex));

    bool added = addToDosingQueue(pumpIndex, ml, isAutomatic, currentDosageFactor);
    if (!added) {
      Serial.println("FEHLER: Konnte Auftrag nicht zur Warteschlange hinzufügen!");
      logDosingEvent("QUEUE_ADD_FAILED", "Pumpe=" + String(pumpIndex) + " Queue voll!");
    }
    return;
  }

  logDosingEvent("PUMP_START", "Pumpe=" + String(pumpIndex) + " ml=" + String(ml, 2) + " activePump=-1");

  // Wenn nicht kalibriert und direkte Dosierung
  if (pumps[pumpIndex].mlPerStep <= 0) {
    Serial.print("Fehler: Pumpe ");
    Serial.print(pumps[pumpIndex].name);
    Serial.println(" ist nicht kalibriert");

    // Bei automatischer Dosierung den Zähler reduzieren
    if (isAutomatic && dosageSequenceActive) {
      pendingDosages--;
      Serial.print("Dosierung übersprungen, verbleibende Dosierungen: ");
      Serial.println(pendingDosages);

      // Prüfen ob alle Dosierungen abgeschlossen
      if (pendingDosages <= 0) {
        settings.lastAutoDosage = currentDosageTimestamp;
        saveSettingsToJson();  // Ersetzt EEPROM.put und EEPROM.commit
        dosageSequenceActive = false;
      }
    }
    return;
  }

  // Dosierungswerte speichern
  currentDosageAmount = ml;
  activePumpIndex = pumpIndex;
  currentDosingIsAutomatic = isAutomatic;
  blockCalculations = true;  // Blockiere Berechnungen während Pumpe aktiv

  // Alte Zustandsvariablen für Kompatibilität aktualisieren
  isDispensingRunning = true;

  // Status auf Dosierung setzen
  pumpState = currentDosingIsAutomatic ? PUMP_AUTO_DISPENSING : PUMP_MANUAL_DISPENSING;

  // Status setzen basierend auf Dosierungstyp
  pumpState = isAutomatic ? PUMP_AUTO_DISPENSING : PUMP_MANUAL_DISPENSING;

  // Bei automatischer Dosierung die letzte bekannte Dosierung speichern
  if (isAutomatic) {
    int dosageType = -1;
    switch (pumpIndex) {
      case 0:
        dosageType = DOSAGE_TYPE_CALCIUM;
        lastKnownCaDosage = ml;
        validDosingHistory = true;
        break;
      case 1:
        dosageType = DOSAGE_TYPE_MAGNESIUM;
        break;
      case 2:
        dosageType = DOSAGE_TYPE_KH_DAY;
        lastKnownKHDosage = ml;
        validDosingHistory = true;
        break;
      case 3:
        dosageType = DOSAGE_TYPE_KH_NIGHT;
        lastKnownKHDosage = ml;  // Speichere physische ml
        validDosingHistory = true;
        break;
    }
  }

  // Wenn Anti-Tropf aktiviert ist, erst mit Vorschub beginnen
  if (settings.enableAntiDrip) {
    startAntiDripPrime();
  } else {
    // Ohne Anti-Tropf direkt mit Dosierung beginnen
    startDosing();
  }
}

// Funktion für den Anti-Tropf-Vorschub (Prime)
void startAntiDripPrime() {
  Serial.print("Anti-Tropf: Starte Vorschub für Pumpe ");
  Serial.print(activePumpIndex);
  Serial.print(" (ml: ");
  Serial.print(settings.antiDripML, 4);
  Serial.print(", Geschw.: ");
  Serial.print(settings.antiDripSpeedML, 4);
  Serial.println(" ml/min)");

  // Die aktive Pumpe aktivieren
  activatePump(activePumpIndex);

  if (pumpenStepper[activePumpIndex]) {
    // Verwende die globale Beschleunigung der Pumpe (aus PumpConfig)
    pumpenStepper[activePumpIndex]->setAcceleration(mlPerMin2ToStepsPerSec2(pumps[activePumpIndex].accelerationML, activePumpIndex));

    // Setze Geschwindigkeit aus den Anti-Tropf-Einstellungen
    pumpenStepper[activePumpIndex]->setSpeedInHz(mlPerMinToStepsPerSec(settings.antiDripSpeedML, activePumpIndex));

    // Hardware-Pins konfigurieren
    digitalWrite(DIR_PIN, HIGH);  // Vorwärtsrichtung

    // Bewegung starten
    pumpenStepper[activePumpIndex]->setCurrentPosition(0);
    pumpenStepper[activePumpIndex]->moveTo(mlToSteps(settings.antiDripML, activePumpIndex));
  }
  pumpState = PUMP_ANTI_DRIP_PRIME;
}

// Neue Funktion für den Start der eigentlichen Dosierung
void startDosing() {
  // Anzahl der Schritte berechnen
  int steps = round(currentDosageAmount / pumps[activePumpIndex].mlPerStep);
  targetSteps = steps;

  // Alte Zustandsvariablen für Kompatibilität aktualisieren
  isDispensingRunning = true;

  // Status auf Dosierung setzen
  pumpState = currentDosingIsAutomatic ? PUMP_AUTO_DISPENSING : PUMP_MANUAL_DISPENSING;

  // DELAY 2: Pause vor Position-Reset nach Anti-Tropf Prime
  delay(50);  // 50ms zwischen Prime und Dosing

  // Richtung einstellen (vorwärts)
  digitalWrite(DIR_PIN, HIGH);

  // Die aktive Pumpe aktivieren
  activatePump(activePumpIndex);

  // FastAccelStepper konfigurieren
  if (pumpenStepper[activePumpIndex]) {
    pumpenStepper[activePumpIndex]->setSpeedInHz(mlPerMinToStepsPerSec(pumps[activePumpIndex].speedML, activePumpIndex));

    if (pumps[activePumpIndex].accelerationML > 0) {
      pumpenStepper[activePumpIndex]->setAcceleration(mlPerMin2ToStepsPerSec2(pumps[activePumpIndex].accelerationML, activePumpIndex));
    } else {
      pumpenStepper[activePumpIndex]->setAcceleration(10000);
    }

    // Positionierung auf Null und Bewegung starten
    pumpenStepper[activePumpIndex]->setCurrentPosition(0);
    pumpenStepper[activePumpIndex]->moveTo(steps);
  }

  // Info speichern
  lastPumpOperation = activePumpIndex;
  lastPumpAmount = currentDosageAmount;

  Serial.print("Dosierung gestartet: ");
  Serial.print(currentDosageAmount);
  Serial.print("ml (");
  Serial.print(steps);
  Serial.print(" Schritte) von ");
  Serial.print(pumps[activePumpIndex].name);
  Serial.println(currentDosingIsAutomatic ? " (automatisch)" : " (manuell)");
}

// Neue Funktion für den Anti-Tropf-Rückzug
void startAntiDripRetract() {
  Serial.print("Anti-Tropf: Starte Rückzug für Pumpe ");
  Serial.print(activePumpIndex);
  Serial.print(" (ml: ");
  Serial.print(settings.antiDripML, 4);
  Serial.print(", Geschw.: ");
  Serial.print(settings.antiDripSpeedML, 4);
Serial.println(" ml/min)");

  // DELAY 3: KRITISCH - Pause vor Richtungswechsel
  delay(50);  // 50ms vor DIR_PIN HIGH→LOW Wechsel

  // Die aktive Pumpe aktivieren
  activatePump(activePumpIndex);

  if (pumpenStepper[activePumpIndex]) {
    // Verwende die globale Pumpen-Beschleunigung
    pumpenStepper[activePumpIndex]->setAcceleration(mlPerMin2ToStepsPerSec2(pumps[activePumpIndex].accelerationML, activePumpIndex));

    // Geschwindigkeit aus Anti-Tropf-Einstellungen
    pumpenStepper[activePumpIndex]->setSpeedInHz(mlPerMinToStepsPerSec(settings.antiDripSpeedML, activePumpIndex));

    // Richtung umschalten (Rückwärts)
    digitalWrite(DIR_PIN, LOW);

    // Bewegung starten
    pumpenStepper[activePumpIndex]->setCurrentPosition(0);
    pumpenStepper[activePumpIndex]->moveTo(-mlToSteps(settings.antiDripML, activePumpIndex));
  }
  pumpState = PUMP_ANTI_DRIP_RETRACT;

  Serial.println("Rückzug gestartet");
}

// Funktion zum Abschließen einer Dosierung
void completeDosage() {
  logDosingEvent("COMPLETE_START", "Pumpe=" + String(activePumpIndex) + " pending=" + String(pendingDosages));

  // Pumpe deaktivieren
  digitalWrite(ENABLE_PIN[activePumpIndex], HIGH);

  // Bestimme den Dosierungstyp anhand des Pumpenindex
  int dosageType;
  switch (activePumpIndex) {
    case 0: dosageType = DOSAGE_TYPE_CALCIUM; break;
    case 1: dosageType = DOSAGE_TYPE_MAGNESIUM; break;
    case 2: dosageType = DOSAGE_TYPE_KH_DAY; break;
    case 3: dosageType = DOSAGE_TYPE_KH_NIGHT; break;
    default: dosageType = -1; break;
  }

  // Speichere die Dosierung (inkl. Schwankungskompensation-Faktor)
  if (dosageType >= 0) {
    saveDosage(activePumpIndex, currentDosageAmount, currentDosingIsAutomatic, currentDosageFactor);
  }
  currentDosageFactor = 1.0;  // Faktor zurücksetzen nach Speicherung

  // Container-Level aktualisieren
  settings.containerLevel[activePumpIndex] -= currentDosageAmount;
  if (settings.containerLevel[activePumpIndex] < 0) {
    settings.containerLevel[activePumpIndex] = 0;
  }

  // Speichere den aktualisierten Flüssigkeitsstand
  saveSettingsToJson();

  // Bei automatischer Dosierung: Weitere Abläufe aktualisieren
  if (dosageSequenceActive && currentDosingIsAutomatic) {
    pendingDosages--;
    logDosingEvent("PENDING_DEC", "pending=" + String(pendingDosages));
    Serial.print("Dosierung abgeschlossen, verbleibende Dosierungen: ");
    Serial.println(pendingDosages);

    // Wenn alle geplanten Dosierungen abgeschlossen sind
    if (pendingDosages <= 0) {
      time_t now = getCurrentTime();

      // Wichtig: immer den originalen Zeitstempel verwenden für die Stunde,
      // für die die Dosierung bestimmt war
      settings.lastAutoDosage = currentDosageTimestamp;
      saveSettingsToJson();

      logDosingEvent("SEQ_COMPLETE", "Stunde=" + String(currentDosageHour) + " active=false");
      Serial.print("Dosierungssequenz abgeschlossen für Stunde: ");
      Serial.print(currentDosageHour);
      Serial.print(" um ");
      Serial.println(formatDateTime(now));
      Serial.print("Letzter Dosierungszeitstempel aktualisiert auf: ");
      Serial.println(formatDateTime(settings.lastAutoDosage));

      dosageSequenceActive = false;
    }
  }

  // Alte Zustandsvariablen zurücksetzen für Kompatibilität
  isDispensingRunning = false;

  // Status zurücksetzen
  activePumpIndex = -1;
  targetSteps = 0;
  pumpState = PUMP_IDLE;
  currentDosageAmount = 0.0;
  blockCalculations = false;  // Berechnungen wieder erlauben

  // Erfolgsmeldung an Client senden
  DynamicJsonDocument responseDoc(256);
  responseDoc["type"] = "success";
  responseDoc["message"] = "Dosierung abgeschlossen: " + String(lastPumpAmount) + " ml von " + pumps[lastPumpOperation].name;
  responseDoc["updateDosage"] = true;

  String responseJson;
  serializeJson(responseDoc, responseJson);
  ws.textAll(responseJson);

  // Prüfe, ob wir den nächsten Job starten müssen
  if (needToStartNextJob) {
    needToStartNextJob = false;
    logDosingEvent("NEXT_JOB_FLAG", "needToStartNextJob=true queueEmpty=" + String(queueEmpty ? "true" : "false"));
    delay(10);  // Kurze Verzögerung für Stabilität
    startNextDosingJob();
  }
// Ansonsten starte nächste Dosierung aus der Warteschlange, wenn vorhanden
  else if (!queueEmpty) {
    // DELAY 8: Pause vor Start der nächsten Pumpe
    logDosingEvent("NEXT_JOB_QUEUE", "queueEmpty=false");
    delay(1000);  // 50ms zwischen verschiedenen Pumpen (war vorher 10ms)
    startNextDosingJob();
  }
  else if (pendingMeasurementRecalc) {
    // Kein weiterer Job → verzögerte Messwert-Neuberechnung nachholen
    Serial.println("Nachträgliche Dosierplan-Neuberechnung nach verzögerter Messung");
    calculateConsumption();
    updateDosagePlans(pendingRecalcIsKH, pendingRecalcIsCalcium);
    pendingMeasurementRecalc = false;
    pendingRecalcIsCalcium = false;
    pendingRecalcIsKH = false;
  }
}

// Diese verbesserte Version der updatePumpOperations() Funktion
// berücksichtigt die rückwärts gelaufenen Schritte
void updatePumpOperations() {
  if (activePumpIndex == -1) return;  // Keine Pumpe aktiv

  // Mehr Debug-Informationen
  static int lastState = -1;
  if (pumpState != lastState) {
    Serial.print("Pumpen-Zustand gewechselt zu: ");
    switch (pumpState) {
      case PUMP_IDLE: Serial.println("PUMP_IDLE"); break;
      case PUMP_MANUAL_DISPENSING: Serial.println("PUMP_MANUAL_DISPENSING"); break;
      case PUMP_AUTO_DISPENSING: Serial.println("PUMP_AUTO_DISPENSING"); break;
      case PUMP_ANTI_DRIP_RETRACT: Serial.println("PUMP_ANTI_DRIP_RETRACT"); break;
      case PUMP_ANTI_DRIP_PRIME: Serial.println("PUMP_ANTI_DRIP_PRIME"); break;
      case PUMP_CALIBRATING: Serial.println("PUMP_CALIBRATING"); break;
      default: Serial.println("UNBEKANNT"); break;
    }
    lastState = pumpState;
  }

  // Prüfen, ob der Stepper seine Zielposition erreicht hat
  bool motorStopped = false;
  if (pumpenStepper[activePumpIndex]) {
    motorStopped = pumpenStepper[activePumpIndex]->isRunning() == 0;

    Serial.print("Motor Status: ");
    Serial.print(motorStopped ? "Gestoppt" : "Läuft");
    Serial.print(", Position: ");
    Serial.print(pumpenStepper[activePumpIndex]->getCurrentPosition());
    Serial.print("/");
    Serial.println(pumpenStepper[activePumpIndex]->targetPos());
  }

  if (motorStopped) {
    // Zusätzliche Debugging-Ausgaben für bessere Diagnose
    Serial.print("updatePumpOperations: aktivePumpe=");
    Serial.print(activePumpIndex);
    Serial.print(", Zustand=");
    switch (pumpState) {
      case PUMP_IDLE: Serial.print("PUMP_IDLE"); break;
      case PUMP_MANUAL_DISPENSING: Serial.print("PUMP_MANUAL_DISPENSING"); break;
      case PUMP_AUTO_DISPENSING: Serial.print("PUMP_AUTO_DISPENSING"); break;
      case PUMP_ANTI_DRIP_RETRACT: Serial.print("PUMP_ANTI_DRIP_RETRACT"); break;
      case PUMP_ANTI_DRIP_PRIME: Serial.print("PUMP_ANTI_DRIP_PRIME"); break;
      case PUMP_CALIBRATING: Serial.print("PUMP_CALIBRATING"); break;
      default: Serial.print("UNBEKANNT"); break;
    }
    Serial.println();

    switch (pumpState) {
case PUMP_ANTI_DRIP_PRIME:
        // Anti-Tropf-Vorschub abgeschlossen, jetzt echte Dosierung starten
        Serial.print("Anti-Tropf: Vorschub abgeschlossen, starte eigentliche Dosierung für Pumpe ");
        Serial.println(activePumpIndex);

        // DELAY 4: Pause vor Übergang Prime→Dosing
        delay(50);  // 50ms zwischen Prime und Dosing-Start

        // WICHTIGE ÄNDERUNG: Zustand HIER direkt ändern, bevor wir startDosing aufrufen
        pumpState = currentDosingIsAutomatic ? PUMP_AUTO_DISPENSING : PUMP_MANUAL_DISPENSING;
        Serial.print("Zustand auf ");
        Serial.print(currentDosingIsAutomatic ? "PUMP_AUTO_DISPENSING" : "PUMP_MANUAL_DISPENSING");
        Serial.println(" gesetzt");

        // Starte eigentliche Dosierung
        startDosing();
        break;

      // HIER FEHLT DIE BEHANDLUNG VON PUMP_MANUAL_DISPENSING UND PUMP_AUTO_DISPENSING
case PUMP_MANUAL_DISPENSING:
      case PUMP_AUTO_DISPENSING:
        // Dosierung abgeschlossen, prüfe ob Anti-Tropf-Rückzug aktiviert werden soll
        Serial.print("Dosierung abgeschlossen für Pumpe ");
        Serial.println(activePumpIndex);

        if (settings.enableAntiDrip) {
          // DELAY 5: Pause vor Übergang Dosing→Retract
          delay(50);  // 50ms vor Richtungswechsel
          
          Serial.println("Starte Anti-Tropf-Rückzug...");
          startAntiDripRetract();
        } else {
          // DELAY 6: Pause vor completeDosage ohne Anti-Tropf
          delay(50);  // 50ms vor Abschluss
          
          Serial.println("Ohne Anti-Tropf, schließe Dosierung direkt ab...");
          completeDosage();
        }
        break;

case PUMP_ANTI_DRIP_RETRACT:
        // Anti-Tropf-Rückzug abgeschlossen
        Serial.print("Anti-Tropf: Rückzug abgeschlossen für Pumpe ");
        Serial.println(activePumpIndex);

        // DELAY 7: Pause vor completeDosage nach Retract
        delay(50);  // 50ms vor Dosierung-Abschluss

        // Dosierung abschließen
        completeDosage();
        break;

      case PUMP_CALIBRATING:
        // Kalibrierung abgeschlossen
        digitalWrite(ENABLE_PIN[activePumpIndex], HIGH);  // HIGH = deaktiviert

        // Alte Zustandsvariablen zurücksetzen für Kompatibilität
        isCalibrationRunning = false;

        Serial.print("Kalibrierung abgeschlossen für Pumpe ");
        Serial.print(activePumpIndex);
        Serial.print(" (");
        Serial.print(pumps[activePumpIndex].name);
        Serial.print(") mit ");
        Serial.print(currentCalibrationSteps);
        Serial.println(" Schritten");

        // Status zurücksetzen
        activePumpIndex = -1;
        targetSteps = 0;
        pumpState = PUMP_IDLE;

        // Prüfe, ob wir den nächsten Job starten müssen
        if (needToStartNextJob) {
          needToStartNextJob = false;
          delay(10);  // Kurze Verzögerung für Stabilität
          startNextDosingJob();
        }
        // Ansonsten starte nächste Dosierung aus der Warteschlange, wenn vorhanden
        else if (!queueEmpty) {
          delay(10);  // Kurze Verzögerung für Stabilität
          startNextDosingJob();
        }
        break;
    }
  }
}

// Funktion zum Auffüllen eines Kanisters
void refillContainer(int containerIndex) {
  if (containerIndex < 0 || containerIndex >= 4) return;

  // Fill the container to capacity
  settings.containerLevel[containerIndex] = settings.containerCapacity[containerIndex];

  // Update last refill timestamp
  settings.lastContainerRefill[containerIndex] = getCurrentTime();

  // Speichern in LittleFS verzögert (wird aus WS-Handler aufgerufen → async_tcp Task)
  deferredFlags.pendingSettingsSave = true;

  Serial.print("Kanister ");
  Serial.print(pumps[containerIndex].name);
  Serial.println(" wurde aufgefüllt");
}

bool verifyCalibrationData(int pumpIndex, float expectedMlPerStep, time_t expectedDate) {
  if (pumpIndex < 0 || pumpIndex >= 4) return false;

  // Werte aus der Pumps-Array prüfen (diese wurden aus JSON geladen)
  float storedMlPerStep = pumps[pumpIndex].mlPerStep;
  time_t storedDate = pumps[pumpIndex].lastCalibrationDate;

  // Verifizierung mit Toleranz für Fließkommazahlen
  const float epsilon = 0.000001;
  bool mlPerStepMatch = (abs(storedMlPerStep - expectedMlPerStep) < epsilon);
  bool dateMatch = (storedDate == expectedDate);

  if (!mlPerStepMatch) {
    Serial.print("Verifizierungsfehler: mlPerStep. Erwartet: ");
    Serial.print(expectedMlPerStep, 6);
    Serial.print(", Gespeichert: ");
    Serial.println(storedMlPerStep, 6);
  }

  if (!dateMatch) {
    Serial.print("Verifizierungsfehler: Datum. Erwartet: ");
    Serial.print(formatDateTime(expectedDate));
    Serial.print(", Gespeichert: ");
    Serial.println(formatDateTime(storedDate));
  }

  return mlPerStepMatch && dateMatch;
}

// Kalibrierung einer Pumpe speichern
bool saveCalibration(int pumpIndex, float ml) {
  if (pumpIndex < 0 || pumpIndex >= 4 || ml <= 0) {
    Serial.println("Fehler: Ungültige Parameter für saveCalibration");
    return false;
  }

  // Prüfen ob eine Kalibrierung läuft oder kürzlich abgeschlossen wurde
  int stepsUsed = 0;

  if (activePumpIndex == pumpIndex && isCalibrationRunning) {
    // Wenn Kalibrierung noch läuft, abbrechen
    Serial.println("Kalibrierung läuft noch, bitte warten Sie bis sie abgeschlossen ist.");
    return false;
  } else if (currentCalibrationSteps > 0 && pumpIndex == currentCalibrationPump) {
    // Fallback auf den globalen Zustand
    stepsUsed = currentCalibrationSteps;
    Serial.print("Verwende abgeschlossene Kalibrierung mit ");
    Serial.print(stepsUsed);
    Serial.println(" Schritten");
  } else {
    Serial.println("Keine aktive oder kürzlich abgeschlossene Kalibrierung gefunden.");
    return false;
  }

  // ml pro Schritt berechnen
  float mlPerStep = ml / stepsUsed;

  // Plausibilitätsprüfung
  if (mlPerStep <= 0.0 || mlPerStep > 0.1) {
    Serial.print("Warnung: Berechneter Kalibrierungswert außerhalb des erwarteten Bereichs: ");
    Serial.print(mlPerStep);
    Serial.println(" ml/Schritt. Fortfahren, aber bitte prüfen.");
  }

  // Aktuelle Zeit holen
  time_t currentTime = getCurrentTime();

  // Werte temporär speichern
  float oldMlPerStep = pumps[pumpIndex].mlPerStep;
  pumps[pumpIndex].mlPerStep = mlPerStep;
  pumps[pumpIndex].lastCalibrationDate = currentTime;

  // In JSON speichern statt EEPROM
  bool success = savePumpsToJson();

  if (!success) {
    Serial.println("Fehler: Speichern in JSON fehlgeschlagen! Kalibrierung konnte nicht gespeichert werden.");
    // Zurücksetzen auf alte Werte
    pumps[pumpIndex].mlPerStep = oldMlPerStep;
    return false;
  }

  // Kalibrierungsstatus vollständig zurücksetzen
  currentCalibrationPump = -1;
  currentCalibrationSteps = 0;
  isCalibrationRunning = false;

  Serial.print("Kalibrierung erfolgreich gespeichert für ");
  Serial.println(pumps[pumpIndex].name);
  Serial.print(" - ");
  Serial.print(mlPerStep, 6);
  Serial.println(" ml/Schritt");
  Serial.print(" - Datum: ");
  Serial.println(formatDateTime(currentTime));

  return true;
}

// =========== SYSTEMEINSTELLUNGEN UND BERECHNUNGEN ===========

// Berechnet, wie viel °dKH pro ml KH-Lösung im gegebenen Aquarium erzeugt wird
float calculateKHPerML() {
  // (1°dKH / Faktor in ml pro 100L) * (100L / Aquariumvolumen)
  return (1.0 / KH_ML_PER_DKH_100L) * (100.0 / settings.aquariumVolume);
}

// Berechnet, wie viel mg/l Calcium pro ml Calcium-Lösung im gegebenen Aquarium erzeugt wird
float calculateCaPerML() {
  // (1mg/l / Faktor in ml pro 100L) * (100L / Aquariumvolumen)
  return (1.0 / CA_ML_PER_MGL_100L) * (100.0 / settings.aquariumVolume);
}

void loadSystemSettings() {
  Serial.println("Lade Systemeinstellungen...");

  // Standardwerte setzen
  SystemSettings tempSettings = {
    DEFAULT_AQUARIUM_VOLUME,
    DEFAULT_TARGET_KH,
    DEFAULT_TARGET_CALCIUM,
    DEFAULT_HISTORY_COUNT,
    false,
    DEFAULT_MAX_DAILY_CHANGE_KH,
    DEFAULT_MAX_DAILY_CHANGE_CALCIUM,
    0,
    DEFAULT_MAGNESIUM_RATIO,
    DEFAULT_KH_NIGHT_START,
    DEFAULT_KH_NIGHT_END,
    DEFAULT_INITIAL_KH_CONSUMPTION,
    DEFAULT_INITIAL_CALCIUM_CONSUMPTION,
    true,  // autoUpdateInitialRates
    { DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY,
      DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY },
    { DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY,
      DEFAULT_CONTAINER_CAPACITY, DEFAULT_CONTAINER_CAPACITY },
    { 0, 0, 0, 0 },
    false,
    DEFAULT_PH_THRESHOLD,
    DEFAULT_ENABLE_ANTI_DRIP,
    DEFAULT_ANTI_DRIP_ML,
    DEFAULT_ANTI_DRIP_SPEED_ML
  };

  // Werte aus Standardeinstellungen kopieren
  settings = tempSettings;

  // Versuche Einstellungen aus JSON zu laden
  if (loadSettingsFromJson()) {
    Serial.println("Systemeinstellungen erfolgreich aus JSON geladen");
  } else {
    Serial.println("Verwende Standardwerte für Systemeinstellungen");
  }

  // Schwankungskompensation-Faktoren laden
  loadDosingFactors();

  Serial.println("Systemeinstellungen laden abgeschlossen");

  // Berechnungen durchführen, wenn die Zeit bereits synchronisiert ist
  if (timeInitialized) {
    Serial.println("Zeit synchronisiert, berechne Verbrauch und Dosierplan...");
    calculateConsumption();
    updateDosagePlans(true, true);
  } else {
    Serial.println("Zeit noch nicht synchronisiert, überspringe Berechnungen");
  }
}

void saveSystemSettings(float aquariumVolume, float targetKH, float targetCalcium, int historyCount,
                        float maxDailyChangeKH, float maxDailyChangeCalcium,
                        float magnesiumRatio, int khNightStart, int khNightEnd,
                        float initialKHConsumption, float initialCalciumConsumption,
                        bool autoUpdateInitialRates,
                        float containerCapacity0, float containerCapacity1,
                        float containerCapacity2, float containerCapacity3,
                        bool usePhBasedKHDosing, float phThresholdForKHNight,
                        bool enableAntiDrip, float antiDripML, float antiDripSpeedML, int newTimeOffset) {
  // Werte speichern
  settings.aquariumVolume = aquariumVolume;
  settings.targetKH = targetKH;
  settings.targetCalcium = targetCalcium;
  settings.historyCount = historyCount;
  settings.maxDailyChangeKH = maxDailyChangeKH;
  settings.maxDailyChangeCalcium = maxDailyChangeCalcium;
  settings.magnesiumRatio = magnesiumRatio;
  settings.khNightStart = khNightStart;
  settings.khNightEnd = khNightEnd;
  settings.initialKHConsumption = initialKHConsumption;
  settings.initialCalciumConsumption = initialCalciumConsumption;
  settings.autoUpdateInitialRates = autoUpdateInitialRates;
  settings.usePhBasedKHDosing = usePhBasedKHDosing;
  settings.phThresholdForKHNight = phThresholdForKHNight;

  // Anti-Tropf-Einstellungen speichern
  settings.enableAntiDrip = enableAntiDrip;
  settings.antiDripML = antiDripML;
  settings.antiDripSpeedML = antiDripSpeedML;

  // Zeit-Offset nur im RAM setzen (LittleFS-Write wird deferred)
  timeOffset = newTimeOffset;

  // Container capacities speichern
  settings.containerCapacity[0] = containerCapacity0;
  settings.containerCapacity[1] = containerCapacity1;
  settings.containerCapacity[2] = containerCapacity2;
  settings.containerCapacity[3] = containerCapacity3;

  // Wenn neue Kapazität kleiner als aktueller Füllstand, Füllstand anpassen
  for (int i = 0; i < 4; i++) {
    if (settings.containerLevel[i] > settings.containerCapacity[i]) {
      settings.containerLevel[i] = settings.containerCapacity[i];
    }
  }

  // Settings speichern + Dosierplan neu berechnen → beides in Queue
  deferredFlags.pendingSettingsSave = true;
  TaskItem t; t.type = TASK_UPDATE_DOSAGE_PLANS; t.boolParam2 = true; t.boolParam3 = true;
  enqueueTask(t);

  Serial.println("Systemeinstellungen aktualisiert (deferred save + recalc)");
}

// Automatische Dosierung aktivieren/deaktivieren
// HINWEIS: Wird jetzt nur noch für nicht-WS Aufrufe (z.B. aus loop) verwendet.
// Im WS-Handler wird stattdessen direkt das Flag + deferred gesetzt.
void setAutoDosing(bool enabled) {
  settings.autoDosing = enabled;
  deferredFlags.pendingSettingsSave = true;

  Serial.printf("Automatische Dosierung %s\n", enabled ? "aktiviert" : "deaktiviert");

  TaskItem t; t.type = TASK_UPDATE_DOSAGE_PLANS; t.boolParam2 = true; t.boolParam3 = true;
  enqueueTask(t);
}

// =========== WiFi-Konfiguration Funktionen ===========

// WiFi-Konfiguration aus LittleFS laden
bool loadWiFiConfig() {
  if (!LittleFS.exists(WIFI_CONFIG_FILE)) {
    Serial.println("WiFi-Konfiguration nicht gefunden");
    return false;
  }

  File file = LittleFS.open(WIFI_CONFIG_FILE, FILE_READ);
  if (!file) {
    Serial.println("Fehler beim Öffnen der WiFi-Konfiguration");
    return false;
  }

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("Fehler beim Parsen der WiFi-Konfiguration: ");
    Serial.println(error.c_str());
    return false;
  }

  wifiSSID = doc["ssid"].as<String>();
  wifiPassword = doc["password"].as<String>();
  wifiConfigured = doc["configured"] | false;

  Serial.println("WiFi-Konfiguration geladen:");
  Serial.print("  SSID: ");
  Serial.println(wifiSSID);
  Serial.print("  Konfiguriert: ");
  Serial.println(wifiConfigured ? "Ja" : "Nein");

  return wifiConfigured;
}

// WiFi-Konfiguration in LittleFS speichern
bool saveWiFiConfig(String ssid, String password) {
  DynamicJsonDocument doc(512);
  doc["ssid"] = ssid;
  doc["password"] = password;
  doc["configured"] = true;

  File file = LittleFS.open(WIFI_CONFIG_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("Fehler beim Speichern der WiFi-Konfiguration");
    return false;
  }

  serializeJson(doc, file);
  file.close();

  wifiSSID = ssid;
  wifiPassword = password;
  wifiConfigured = true;

  Serial.println("WiFi-Konfiguration gespeichert:");
  Serial.print("  SSID: ");
  Serial.println(ssid);

  return true;
}

// Access Point starten
void startAccessPoint() {
  Serial.println("=== Starte Access Point Modus ===");
  Serial.print("AP-Name: ");
  Serial.println(apSSID);

  // WiFi im AP-Modus starten (ohne Passwort für einfaches Setup)
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSSID);

  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP IP-Adresse: ");
  Serial.println(apIP);
  Serial.println("Verbinde dich mit dem Access Point und öffne http://192.168.4.1");

  isAPMode = true;
  systemStatus = "AP-Modus: " + String(apSSID);
}

void calculateConsumption() {
  // Aktuelle Zeit holen
  time_t now = getCurrentTime();

  // Umrechnungsfaktoren
  float khPerMl = calculateKHPerML();
  float caPerMl = calculateCaPerML();

  // KH-Messungen holen
  int khMeasCount;
  Measurement* khMeasurements = getAllMeasurements(false, khMeasCount);

  // Nach Zeitstempel sortieren (älteste zuerst)
  sortMeasurementsByTimestamp(khMeasurements, khMeasCount);

  // KH-Dosierungen holen (kombiniert über getAllKHDosages)
  int khDosCount;
  Dosage* khDosages = getAllKHDosages(khDosCount);
  sortDosagesByTimestamp(khDosages, khDosCount);

  // Calcium-Messungen holen
  int caMeasCount;
  Measurement* caMeasurements = getAllMeasurements(true, caMeasCount);

  // Nach Zeitstempel sortieren
  sortMeasurementsByTimestamp(caMeasurements, caMeasCount);

  // Calcium und Magnesium Dosierungen holen (kombiniert über getAllCalciumDosages)
  int caDosCount;
  Dosage* combinedCaDosages = getAllCalciumDosages(caDosCount);
  sortDosagesByTimestamp(combinedCaDosages, caDosCount);

  // Prüfen, ob erste Messung
  bool isFirstKHMeasurement = (khMeasCount == 1);
  bool isFirstCaMeasurement = (caMeasCount == 1);

  //================================================
  // KH-Verbrauchsberechnung mit korrekter Zeitbasis
  //================================================

  // Intelligente KH-Verbrauchsberechnung basierend auf verfügbaren Messungen
  if (khMeasCount == 0) {
    // Keine Messung: Verwende Default-Werte
    dailyKHConsumption = settings.initialKHConsumption;
    Serial.print("Keine KH-Messung vorhanden, verwende initiale Verbrauchsrate: ");
    Serial.print(dailyKHConsumption);
    Serial.println(" ml/Tag");
  } else if (khMeasCount == 1) {
    // Nur 1 Messung: Verwende Default-Werte
    dailyKHConsumption = settings.initialKHConsumption;
    Serial.print("Nur 1 KH-Messung vorhanden, verwende initiale Verbrauchsrate: ");
    Serial.print(dailyKHConsumption);
    Serial.println(" ml/Tag");
  } else if (khMeasCount == 2) {
    // Genau 2 Messungen: Direkte Berechnung ohne Gewichtung
    time_t startTime = khMeasurements[0].timestamp;
    time_t endTime = khMeasurements[1].timestamp;
    float startValue = khMeasurements[0].value;
    float endValue = khMeasurements[1].value;
    float days = (endTime - startTime) / 86400.0;

    if (days > 0.0) {
      // Berechne Summe aller Dosierungen im Zeitraum
      float dosagedKH = 0.0;
      for (int d = 0; d < khDosCount; d++) {
        if (khDosages[d].timestamp >= startTime && khDosages[d].timestamp < endTime) {
          if (khDosages[d].dosageType == DOSAGE_TYPE_KH_NIGHT) {
            dosagedKH += khDosages[d].amount * 2.0;
          } else {
            dosagedKH += khDosages[d].amount;
          }
        }
      }

      float actualChange = endValue - startValue;
      dailyKHConsumption = (dosagedKH - (actualChange / khPerMl)) / days;

      if (dailyKHConsumption <= 0 || isnan(dailyKHConsumption)) {
        dailyKHConsumption = settings.initialKHConsumption;
      }

      Serial.print("2 KH-Messungen: Direkte Berechnung = ");
      Serial.print(dailyKHConsumption);
      Serial.println(" ml/Tag");
    } else {
      dailyKHConsumption = settings.initialKHConsumption;
      Serial.println("Ungültiger Zeitraum bei 2 Messungen, verwende initiale Rate");
    }
  } else {
    // 3+ Messungen: Gewichtungsberechnung mit verbesserter Logik
    int usedMeasurements = min(khMeasCount, settings.historyCount);
    int startIdx = max(0, khMeasCount - usedMeasurements);
    int actualIntervals = usedMeasurements - 1;

    // Sicherheitsprüfung: actualIntervals muss positiv sein
    if (actualIntervals <= 0) {
      dailyKHConsumption = settings.initialKHConsumption;
      Serial.println("FEHLER: Ungültige Anzahl Intervalle bei KH-Berechnung, verwende initiale Rate");
    } else {
      Serial.print("KH-Gewichtungsberechnung: ");
      Serial.print(usedMeasurements);
      Serial.print(" Messungen, ");
      Serial.print(actualIntervals);
      Serial.println(" Intervalle");

      // Arrays für Zwischenergebnisse anlegen
      float* intervalConsumptions = psram_new_array<float>(actualIntervals);
      int validIntervals = 0;

    // SCHRITT 1: Intervall-Verbrauch berechnen
    for (int i = startIdx + 1; i < khMeasCount; i++) {
      time_t startTime = khMeasurements[i - 1].timestamp;
      time_t endTime = khMeasurements[i].timestamp;
      float startValue = khMeasurements[i - 1].value;
      float endValue = khMeasurements[i].value;
      float days = (endTime - startTime) / 86400.0;

      if (days > 0.0) {
        float dosagedKH = 0.0;
        for (int d = 0; d < khDosCount; d++) {
          if (khDosages[d].timestamp >= startTime && khDosages[d].timestamp < endTime) {
            if (khDosages[d].dosageType == DOSAGE_TYPE_KH_NIGHT) {
              dosagedKH += khDosages[d].amount * 2.0;
            } else {
              dosagedKH += khDosages[d].amount;
            }
          }
        }

        float actualChange = endValue - startValue;
        float consumption = (dosagedKH - (actualChange / khPerMl)) / days;
        intervalConsumptions[validIntervals] = consumption;
        validIntervals++;
      }
    }

    // SCHRITT 2: Gleichgewichtung aller Intervalle
    if (validIntervals > 0) {
      if (validIntervals == 1) {
        // Nur 1 Intervall: Verwende direkt ohne Gewichtung
        dailyKHConsumption = intervalConsumptions[0];
        Serial.println("Nur 1 gültiges Intervall, verwende direkt");
      } else {
        // Mehrere Intervalle: Einfache Gleichgewichtung (Durchschnitt)
        float finalConsumption = 0.0;

        for (int i = 0; i < validIntervals; i++) {
          finalConsumption += intervalConsumptions[i];
        }

        dailyKHConsumption = finalConsumption / validIntervals;
        Serial.print("KH-Gleichgewichtungsberechnung abgeschlossen (");
        Serial.print(validIntervals);
        Serial.print(" Intervalle): ");
        Serial.print(dailyKHConsumption);
        Serial.println(" ml/Tag");
      }
    } else {
      dailyKHConsumption = settings.initialKHConsumption;
      Serial.println("Keine gültigen Intervalle, verwende initiale Rate");
    }

    // Speicher freigeben
    psram_delete_array(intervalConsumptions);
    }  // Ende der Sicherheitsprüfung (actualIntervals > 0)
  }

  // Sicherheitscheck: Falls dailyKHConsumption immer noch ungültig ist
  if (isnan(dailyKHConsumption) || dailyKHConsumption <= 0) {
    dailyKHConsumption = settings.initialKHConsumption;
    Serial.print("WARNUNG: KH-Verbrauchsberechnung ergab ungültigen Wert, verwende Fallback: ");
    Serial.print(dailyKHConsumption);
    Serial.println(" ml/Tag");
  }

  // Initiale KH-Rate automatisch aktualisieren
  if (settings.autoUpdateInitialRates && khMeasCount >= 2
      && dailyKHConsumption > 0 && !isnan(dailyKHConsumption)
      && dailyKHConsumption != settings.initialKHConsumption) {
    Serial.printf("Auto-Update initiale KH-Rate: %.1f -> %.1f ml/Tag\n",
                  settings.initialKHConsumption, dailyKHConsumption);
    settings.initialKHConsumption = dailyKHConsumption;
    deferredFlags.pendingSettingsSave = true;
  }

  //================================================
  // Calcium-Verbrauchsberechnung mit korrekter Zeitbasis
  //================================================

  // Intelligente Calcium-Verbrauchsberechnung basierend auf verfügbaren Messungen
  if (caMeasCount == 0) {
    // Keine Messung: Verwende Default-Werte
    dailyCalciumConsumption = settings.initialCalciumConsumption;
    Serial.print("Keine Calcium-Messung vorhanden, verwende initiale Verbrauchsrate: ");
    Serial.print(dailyCalciumConsumption);
    Serial.println(" ml/Tag");
  } else if (caMeasCount == 1) {
    // Nur 1 Messung: Verwende Default-Werte
    dailyCalciumConsumption = settings.initialCalciumConsumption;
    Serial.print("Nur 1 Calcium-Messung vorhanden, verwende initiale Verbrauchsrate: ");
    Serial.print(dailyCalciumConsumption);
    Serial.println(" ml/Tag");
  } else if (caMeasCount == 2) {
    // Genau 2 Messungen: Direkte Berechnung ohne Gewichtung
    time_t startTime = caMeasurements[0].timestamp;
    time_t endTime = caMeasurements[1].timestamp;
    float startValue = caMeasurements[0].value;
    float endValue = caMeasurements[1].value;
    float days = (endTime - startTime) / 86400.0;

    if (days > 0.0) {
      // Berechne Summe aller Dosierungen im Zeitraum
      float dosagedCa = 0.0;
      for (int d = 0; d < caDosCount; d++) {
        if (combinedCaDosages[d].timestamp >= startTime && combinedCaDosages[d].timestamp < endTime && combinedCaDosages[d].dosageType == DOSAGE_TYPE_CALCIUM) {
          dosagedCa += combinedCaDosages[d].amount;
        }
      }

      float actualChange = endValue - startValue;
      dailyCalciumConsumption = (dosagedCa - (actualChange / caPerMl)) / days;

      if (dailyCalciumConsumption <= 0 || isnan(dailyCalciumConsumption)) {
        dailyCalciumConsumption = settings.initialCalciumConsumption;
      }

      Serial.print("2 Calcium-Messungen: Direkte Berechnung = ");
      Serial.print(dailyCalciumConsumption);
      Serial.println(" ml/Tag");
    } else {
      dailyCalciumConsumption = settings.initialCalciumConsumption;
      Serial.println("Ungültiger Zeitraum bei 2 Messungen, verwende initiale Rate");
    }
  } else {
    // 3+ Messungen: Gewichtungsberechnung mit verbesserter Logik
    int usedMeasurements = min(caMeasCount, settings.historyCount);
    int startIdx = max(0, caMeasCount - usedMeasurements);
    int actualIntervals = usedMeasurements - 1;

    // Sicherheitsprüfung: actualIntervals muss positiv sein
    if (actualIntervals <= 0) {
      dailyCalciumConsumption = settings.initialCalciumConsumption;
      Serial.println("FEHLER: Ungültige Anzahl Intervalle bei Calcium-Berechnung, verwende initiale Rate");
    } else {
      Serial.print("Ca-Gewichtungsberechnung: ");
      Serial.print(usedMeasurements);
      Serial.print(" Messungen, ");
      Serial.print(actualIntervals);
      Serial.println(" Intervalle");

      // Arrays für Zwischenergebnisse anlegen
      float* intervalConsumptions = psram_new_array<float>(actualIntervals);
      int validIntervals = 0;

    // SCHRITT 1: Intervall-Verbrauch berechnen
    for (int i = startIdx + 1; i < caMeasCount; i++) {
      time_t startTime = caMeasurements[i - 1].timestamp;
      time_t endTime = caMeasurements[i].timestamp;
      float startValue = caMeasurements[i - 1].value;
      float endValue = caMeasurements[i].value;
      float days = (endTime - startTime) / 86400.0;

      if (days > 0.0) {
        float dosagedCa = 0.0;
        for (int d = 0; d < caDosCount; d++) {
          if (combinedCaDosages[d].timestamp >= startTime && combinedCaDosages[d].timestamp < endTime && combinedCaDosages[d].dosageType == DOSAGE_TYPE_CALCIUM) {
            dosagedCa += combinedCaDosages[d].amount;
          }
        }

        float actualChange = endValue - startValue;
        float consumption = (dosagedCa - (actualChange / caPerMl)) / days;
        intervalConsumptions[validIntervals] = consumption;
        validIntervals++;
      }
    }

    // SCHRITT 2: Gleichgewichtung aller Intervalle
    if (validIntervals > 0) {
      if (validIntervals == 1) {
        // Nur 1 Intervall: Verwende direkt ohne Gewichtung
        dailyCalciumConsumption = intervalConsumptions[0];
        Serial.println("Nur 1 gültiges Intervall, verwende direkt");
      } else {
        // Mehrere Intervalle: Einfache Gleichgewichtung (Durchschnitt)
        float finalConsumption = 0.0;

        for (int i = 0; i < validIntervals; i++) {
          finalConsumption += intervalConsumptions[i];
        }

        dailyCalciumConsumption = finalConsumption / validIntervals;
        Serial.print("Ca-Gleichgewichtungsberechnung abgeschlossen (");
        Serial.print(validIntervals);
        Serial.print(" Intervalle): ");
        Serial.print(dailyCalciumConsumption);
        Serial.println(" ml/Tag");
      }
    } else {
      dailyCalciumConsumption = settings.initialCalciumConsumption;
      Serial.println("Keine gültigen Intervalle, verwende initiale Rate");
    }

    // Speicher freigeben
    psram_delete_array(intervalConsumptions);
    }  // Ende der Sicherheitsprüfung (actualIntervals > 0)
  }

  // Sicherheitscheck: Falls dailyCalciumConsumption immer noch ungültig ist
  if (isnan(dailyCalciumConsumption) || dailyCalciumConsumption <= 0) {
    dailyCalciumConsumption = settings.initialCalciumConsumption;
    Serial.print("WARNUNG: Ca-Verbrauchsberechnung ergab ungültigen Wert, verwende Fallback: ");
    Serial.print(dailyCalciumConsumption);
    Serial.println(" ml/Tag");
  }

  // Initiale Ca-Rate automatisch aktualisieren
  if (settings.autoUpdateInitialRates && caMeasCount >= 2
      && dailyCalciumConsumption > 0 && !isnan(dailyCalciumConsumption)
      && dailyCalciumConsumption != settings.initialCalciumConsumption) {
    Serial.printf("Auto-Update initiale Ca-Rate: %.1f -> %.1f ml/Tag\n",
                  settings.initialCalciumConsumption, dailyCalciumConsumption);
    settings.initialCalciumConsumption = dailyCalciumConsumption;
    deferredFlags.pendingSettingsSave = true;
  }

  // Speicher freigeben
  psram_delete_array(khMeasurements);
  psram_delete_array(khDosages);
  psram_delete_array(caMeasurements);
  psram_delete_array(combinedCaDosages);
}

void calculateKHDosagePlan() {
  // Aktuelle Zeit holen
  time_t now = getCurrentTime();
  Serial.println("==== KH-Dosierplan-Berechnung (KORRIGIERT MIT LIMITS) ====");
  Serial.print("Aktuelle Zeit: ");
  Serial.println(formatDateTime(now));

  // Letzte KH-Messung holen
  time_t lastKHTimestamp = getLastMeasurementTimestamp(false);  // KH

  if (lastKHTimestamp == 0) {
    Serial.println("⚠️ Keine KH-Messungen gefunden - verwende initiale Verbrauchsrate");
    Serial.print("Initiale KH-Verbrauchsrate: ");
    Serial.print(settings.initialKHConsumption);
    Serial.println(" ml/Tag");

    // Setze dailyKHConsumption auf initiale Rate, falls noch nicht gesetzt
    if (dailyKHConsumption <= 0) {
      dailyKHConsumption = settings.initialKHConsumption;
    }
  } else {
    Serial.print("Letzte KH-Messung: ");
    Serial.println(formatDateTime(lastKHTimestamp));
  }

  // Eingangswerte prüfen und debuggen
  Serial.println("------ EINGANGSWERTE ------");
  Serial.print("dailyKHConsumption: ");
  Serial.print(dailyKHConsumption);
  Serial.println(" ml/Tag");
  Serial.print("settings.targetKH: ");
  Serial.print(settings.targetKH);
  Serial.println(" °dKH");
  Serial.print("settings.maxDailyChangeKH: ");
  Serial.print(settings.maxDailyChangeKH);
  Serial.println(" °dKH/Tag");

  // Umrechnungsfaktor holen und debuggen
  float khPerMl = calculateKHPerML();
  Serial.print("Umrechnungsfaktor khPerMl: ");
  Serial.print(khPerMl, 6);
  Serial.println(" °dKH/ml");

  if (khPerMl <= 0) {
    Serial.println("FEHLER: khPerMl ist 0 oder negativ! Abbruch der Berechnung.");
    return;
  }

  // Startwert bestimmen
  float startKH = getLatestValue(false);

  // WICHTIG: Wenn keine Messungen vorhanden, Target als Start verwenden
  if (startKH == 0 && lastKHTimestamp == 0) {
    startKH = settings.targetKH;
    Serial.println("⚠️ Keine Messwerte - verwende Target-KH als Startwert");
  }

  float targetKH = settings.targetKH;
  float totalDifference = abs(targetKH - startKH);
  bool isIncrease = (targetKH > startKH);

  Serial.print("KH-Startwert: ");
  Serial.print(startKH);
  Serial.println(" °dKH");
  Serial.print("Ziel-KH: ");
  Serial.print(targetKH);
  Serial.println(" °dKH");
  Serial.print("Differenz: ");
  Serial.print(totalDifference);
  Serial.print(" °dKH (");
  Serial.print(isIncrease ? "ERHÖHUNG" : "SENKUNG");
  Serial.println(")");

  // Wenn bereits am Ziel, nur Erhaltungsdosierung
  if (totalDifference < 0.05) {
    Serial.println("Ziel bereits erreicht, nur Erhaltungsdosierung");

    // Plan initialisieren
    memset(khDosagePlan, 0, sizeof(khDosagePlan));
    khDosagePlanSize = 0;

    // Nur Erhaltungsdosierung hinzufügen
    float maintenanceKHDosage = dailyKHConsumption / 12.0;
    if (maintenanceKHDosage <= 0) {
      maintenanceKHDosage = settings.initialKHConsumption / 12.0;
    }

    // Plan IMMER mit KH-Tag erstellen (pH-Entscheidung erfolgt zur Laufzeit)
    bool maintenanceIsNight = false;  // IMMER KH-Tag im Plan

    khDosagePlan[0].date = 0;  // 0 = Erhaltungsdosierung
    khDosagePlan[0].dosage = maintenanceKHDosage;
    khDosagePlan[0].projectedValue = startKH;
    khDosagePlan[0].isNightDosage = maintenanceIsNight;
    khDosagePlan[0].isMaintenanceDose = true;
    khDosagePlanSize = 1;

    lastKHPlanCalculation = now;
    saveKHDosagePlan();
    return;
  }

  // Natürlicher Verbrauch berechnen
  float naturalConsumptionDKH = (dailyKHConsumption / 12.0) * khPerMl;
  if (naturalConsumptionDKH <= 0) {
    naturalConsumptionDKH = (settings.initialKHConsumption / 12.0) * khPerMl;
  }

  Serial.print("Natürlicher Verbrauch: ");
  Serial.print(naturalConsumptionDKH);
  Serial.println(" °dKH pro Intervall");

  // Eingestelltes Maximum aus den Settings
  float settingsMaxChangePerInterval = settings.maxDailyChangeKH / 12.0;

  // Physikalisches Maximum bei Senkung
  float physicalMaxDecreasePerInterval = naturalConsumptionDKH;

  // Tatsächlich verwendetes Maximum bestimmen
  float maxChangePerInterval;

  if (isIncrease) {
    // ERHÖHUNG: Unbegrenzt nach oben (nur durch Einstellungen begrenzt)
    maxChangePerInterval = settingsMaxChangePerInterval;
    Serial.print("Erhöhung: Nutze Einstellungs-Maximum = ");
    Serial.print(maxChangePerInterval);
    Serial.println(" °dKH pro Intervall");

  } else {
    // SENKUNG: Durch natürlichen Verbrauch begrenzt
    maxChangePerInterval = min(settingsMaxChangePerInterval, physicalMaxDecreasePerInterval);

    Serial.print("Senkung: Einstellungs-Maximum = ");
    Serial.print(settingsMaxChangePerInterval);
    Serial.print(" °dKH, Physikalisches Maximum = ");
    Serial.print(physicalMaxDecreasePerInterval);
    Serial.print(" °dKH, Verwendet = ");
    Serial.print(maxChangePerInterval);
    Serial.println(" °dKH pro Intervall");

    // Warnung bei Begrenzung durch physikalisches Limit
    if (physicalMaxDecreasePerInterval < settingsMaxChangePerInterval) {
      Serial.println("⚠️  WARNUNG: Senkung durch natürlichen Verbrauch begrenzt!");
      Serial.print("Eingestelltes Maximum: ");
      Serial.print(settingsMaxChangePerInterval * 12);
      Serial.println(" °dKH/Tag");
      Serial.print("Tatsächlich möglich: ");
      Serial.print(physicalMaxDecreasePerInterval * 12);
      Serial.println(" °dKH/Tag");
      Serial.println("Grund: Natürlicher Verbrauch begrenzt die Senkungsgeschwindigkeit");

      // Warnung an WebSocket senden
      DynamicJsonDocument doc(512);
      doc["type"] = "warning";
      doc["message"] = "KH-Senkung durch natürlichen Verbrauch begrenzt";
      doc["settingsMax"] = settingsMaxChangePerInterval * 12;
      doc["physicalMax"] = physicalMaxDecreasePerInterval * 12;
      doc["parameter"] = "KH";
      String json;
      serializeJson(doc, json);
      ws.textAll(json);
    }
  }

  // KORRIGIERTE Berechnung (mit Debug)
  Serial.println("------ BERECHNUNGSSCHRITTE ------");
  Serial.print("1. Erste Berechnung: totalDifference=");
  Serial.print(totalDifference, 4);
  Serial.print(" °dKH, maxChangePerInterval=");
  Serial.print(maxChangePerInterval, 4);
  Serial.println(" °dKH");

  int numDosages;
  float changePerDosage;

  if (!isIncrease) {
    // SENKUNG: Korrekte Berechnung mit vollständigen Verbrauchs-Einheiten
    float fullUnits = floor(totalDifference / maxChangePerInterval);
    float remainder = totalDifference - (fullUnits * maxChangePerInterval);

    Serial.print("2. SENKUNG - Vollständige Einheiten: ");
    Serial.print(fullUnits);
    Serial.print(" × ");
    Serial.print(maxChangePerInterval, 4);
    Serial.print(" = ");
    Serial.print(fullUnits * maxChangePerInterval, 4);
    Serial.println(" °dKH");

    Serial.print("   Rest: ");
    Serial.print(remainder, 4);
    Serial.println(" °dKH");

    if (remainder > 0.001) {
      numDosages = (int)fullUnits + 1;
      Serial.print("   Benötigte Dosierungen: ");
      Serial.print((int)fullUnits);
      Serial.println(" vollständige + 1 partielle");
    } else {
      numDosages = (int)fullUnits;
      Serial.print("   Benötigte Dosierungen: ");
      Serial.print((int)fullUnits);
      Serial.println(" vollständige");
    }

    // Keine gleichmäßige Aufteilung - verwende natürliche Einheiten
    changePerDosage = maxChangePerInterval;

  } else {
    // ERHÖHUNG: Bisherige bewährte Logik beibehalten
    numDosages = ceil(totalDifference / maxChangePerInterval);
    changePerDosage = totalDifference / numDosages;

    Serial.print("2. ERHÖHUNG - Initial numDosages = ceil(");
    Serial.print(totalDifference, 4);
    Serial.print(" / ");
    Serial.print(maxChangePerInterval, 4);
    Serial.print(") = ");
    Serial.println(numDosages);

    Serial.print("4. ERHÖHUNG - Normale Aufteilung: ");
    Serial.print(totalDifference, 4);
    Serial.print(" / ");
    Serial.print(numDosages);
    Serial.print(" = ");
    Serial.print(changePerDosage, 4);
    Serial.println(" °dKH pro Dosierung");
  }

  Serial.println("------ FINALE WERTE ------");
  Serial.print("Finale Dosierungen: ");
  Serial.println(numDosages);
  Serial.print("Finale Änderung pro Dosierung: ");
  Serial.print(changePerDosage, 4);
  Serial.println(" °dKH");
  Serial.print("Geschätzte Dauer: ");
  Serial.print((float)numDosages * 2.0 / 24.0, 1);
  Serial.println(" Tage");

  // Berechne die nächste Dosierzeit für KH
  time_t nextKHDosingTime = 0;
  int khHour = getHourFromTime(lastKHTimestamp);
  int khMinute = Europe_Berlin.minute(lastKHTimestamp);

  // Finde die nächste gerade Stunde für Dosierung
  int khNextDosingHour;
  if (khHour % 2 == 0) {
    if (khMinute < 10) {
      khNextDosingHour = khHour;
    } else {
      khNextDosingHour = (khHour + 2) % 24;
    }
  } else {
    khNextDosingHour = (khHour + 1) % 24;
  }

  int khDaysToAdd = 0;
  if (khNextDosingHour < khHour) {
    khDaysToAdd = 1;
  }

  nextKHDosingTime = getTimeForHourOnDay(lastKHTimestamp, khDaysToAdd, khNextDosingHour);

  // Plan initialisieren
  memset(khDosagePlan, 0, sizeof(khDosagePlan));
  khDosagePlanSize = 0;

  // Dosierungen erstellen
  int khPlanEntries = 0;
  time_t currentDosageTime = nextKHDosingTime;

  if (isIncrease) {
    // ERHÖHUNG: Anpassung zur Erhaltung hinzufügen
    float adjustmentPerDosage = changePerDosage / khPerMl;
    float maintenanceDosage = dailyKHConsumption / 12.0;
    if (maintenanceDosage <= 0) {
      maintenanceDosage = settings.initialKHConsumption / 12.0;
    }

    Serial.println("------ ERHÖHUNGS-DOSIERUNGEN ------");
    Serial.print("Anpassung pro Dosierung: ");
    Serial.print(adjustmentPerDosage);
    Serial.println(" ml");
    Serial.print("Erhaltung pro Dosierung: ");
    Serial.print(maintenanceDosage);
    Serial.println(" ml");

    for (int i = 0; i < numDosages && khPlanEntries < MAX_KH_PLAN_ENTRIES - 1; i++) {
      float totalDosage = adjustmentPerDosage + maintenanceDosage;
      float projectedKH = startKH + (i + 1) * changePerDosage;

      // Plan IMMER mit KH-Tag erstellen (pH-Entscheidung erfolgt zur Laufzeit)
      bool isNightDosage = false;  // IMMER KH-Tag im Plan

      khDosagePlan[khPlanEntries].date = currentDosageTime;
      khDosagePlan[khPlanEntries].dosage = totalDosage;
      khDosagePlan[khPlanEntries].projectedValue = projectedKH;
      khDosagePlan[khPlanEntries].isNightDosage = isNightDosage;
      khDosagePlan[khPlanEntries].isMaintenanceDose = false;
      khPlanEntries++;

      currentDosageTime += (2 * 3600);  // Nächste Dosierung in 2 Stunden
    }

  } else {
    // SENKUNG: KORRIGIERTE Erhaltung reduzieren oder stoppen
    float maintenanceDosage = dailyKHConsumption / 12.0;
    if (maintenanceDosage <= 0) {
      maintenanceDosage = settings.initialKHConsumption / 12.0;
    }

    Serial.println("------ SENKUNGS-DOSIERUNGEN (KORRIGIERT) ------");
    Serial.print("Normale Erhaltung: ");
    Serial.print(maintenanceDosage, 2);
    Serial.println(" ml");
    Serial.print("Natürlicher Verbrauch: ");
    Serial.print(naturalConsumptionDKH, 4);
    Serial.println(" °dKH pro Intervall");

    float remainingReduction = totalDifference;

    for (int i = 0; i < numDosages && khPlanEntries < MAX_KH_PLAN_ENTRIES - 1; i++) {
      Serial.print("=== DOSIERUNG ");
      Serial.print(i + 1);
      Serial.println(" ===");

      // Berechne die tatsächlich benötigte Reduktion für diese Dosierung
      float requiredReduction = min(remainingReduction, maxChangePerInterval);
      float finalDosage = 0;

      Serial.print("Verbleibende Gesamtreduktion: ");
      Serial.print(remainingReduction, 4);
      Serial.println(" °dKH");
      Serial.print("Reduktion diese Dosierung: ");
      Serial.print(requiredReduction, 4);
      Serial.println(" °dKH");

      if (requiredReduction <= naturalConsumptionDKH) {
        Serial.print("✅ Reduktion möglich (");
        Serial.print(requiredReduction, 4);
        Serial.print(" <= ");
        Serial.print(naturalConsumptionDKH, 4);
        Serial.println(")");

        float dosageReduction = requiredReduction / khPerMl;
        Serial.print("Dosierungs-Reduktion: ");
        Serial.print(requiredReduction, 4);
        Serial.print(" °dKH / ");
        Serial.print(khPerMl, 6);
        Serial.print(" = ");
        Serial.print(dosageReduction, 2);
        Serial.println(" ml weniger");

        finalDosage = maintenanceDosage - dosageReduction;
        finalDosage = max(0.0f, finalDosage);

        Serial.print("Finale Dosierung: ");
        Serial.print(maintenanceDosage, 2);
        Serial.print(" - ");
        Serial.print(dosageReduction, 2);
        Serial.print(" = ");
        Serial.print(finalDosage, 2);
        Serial.println(" ml");

        // Aktualisiere verbleibende Reduktion
        remainingReduction -= requiredReduction;
      } else {
        Serial.print("❌ Reduktion NICHT möglich (");
        Serial.print(requiredReduction, 4);
        Serial.print(" > ");
        Serial.print(naturalConsumptionDKH, 4);
        Serial.println(") - stoppe Dosierung");
        finalDosage = 0;

        // Bei unmöglicher Reduktion: verbleibende Reduktion um natürlichen Verbrauch reduzieren
        remainingReduction -= min(remainingReduction, naturalConsumptionDKH);
      }

      // KORRIGIERTE Prognose-Berechnung
      float totalReductionSoFar = totalDifference - remainingReduction;
      float projectedKH = startKH - totalReductionSoFar;
      projectedKH = max(projectedKH, targetKH);

      Serial.print("Prognose-KH: ");
      Serial.print(startKH, 2);
      Serial.print(" - ");
      Serial.print(totalReductionSoFar, 4);
      Serial.print(" = ");
      Serial.print(projectedKH, 3);
      Serial.println(" °dKH");

      khDosagePlan[khPlanEntries].date = currentDosageTime;
      khDosagePlan[khPlanEntries].dosage = finalDosage;
      khDosagePlan[khPlanEntries].projectedValue = projectedKH;
      khDosagePlan[khPlanEntries].isNightDosage = false;  // Senkung meist tagsüber
      khDosagePlan[khPlanEntries].isMaintenanceDose = false;
      khPlanEntries++;

      currentDosageTime += (2 * 3600);  // Nächste Dosierung in 2 Stunden

      // Beende Schleife wenn Ziel erreicht
      if (remainingReduction <= 0.001) {
        Serial.println("Zielwert erreicht - beende Dosierungsplanung");
        break;
      }
    }
  }

  // Erhaltungsdosierung hinzufügen
  float maintenanceKHDosage = dailyKHConsumption / 12.0;
  if (maintenanceKHDosage <= 0) {
    maintenanceKHDosage = settings.initialKHConsumption / 12.0;
  }

  bool maintenanceIsNight = false;  // IMMER KH-Tag im Plan

  khDosagePlan[khPlanEntries].date = 0;  // 0 = Erhaltungsdosierung (unendlich)
  khDosagePlan[khPlanEntries].dosage = maintenanceKHDosage;
  khDosagePlan[khPlanEntries].projectedValue = targetKH;  // Stabilisiert bei Zielwert
  khDosagePlan[khPlanEntries].isNightDosage = maintenanceIsNight;
  khDosagePlan[khPlanEntries].isMaintenanceDose = true;
  khPlanEntries++;

  // Aktualisiere die Anzahl der tatsächlichen Einträge
  khDosagePlanSize = khPlanEntries;

  // Aktualisiere den Zeitstempel der letzten Planberechnung
  lastKHPlanCalculation = now;

  // Plan speichern
  saveKHDosagePlan();

  Serial.println("------ ZUSAMMENFASSUNG ------");
  Serial.print("Anpassungsdosierungen: ");
  Serial.println(khPlanEntries - 1);
  Serial.print("Gesamteinträge im Plan: ");
  Serial.println(khDosagePlanSize);
  Serial.print("Geschätzte Dauer: ");
  Serial.print((float)(khPlanEntries - 1) * 2.0 / 24.0, 1);
  Serial.println(" Tage");
  Serial.println("================================");
}

void calculateCaDosagePlan() {
  // Aktuelle Zeit holen
  time_t now = getCurrentTime();
  Serial.println("==== Calcium-Dosierplan-Berechnung (KORRIGIERT MIT LIMITS) ====");
  Serial.print("Aktuelle Zeit: ");
  Serial.println(formatDateTime(now));

  // Letzte Calcium-Messung holen
  time_t lastCaTimestamp = getLastMeasurementTimestamp(true);  // Calcium

  if (lastCaTimestamp == 0) {
    Serial.println("⚠️ Keine Calcium-Messungen gefunden - verwende initiale Verbrauchsrate");
    Serial.print("Initiale Calcium-Verbrauchsrate: ");
    Serial.print(settings.initialCalciumConsumption);
    Serial.println(" ml/Tag");

    // Setze dailyCalciumConsumption auf initiale Rate, falls noch nicht gesetzt
    if (dailyCalciumConsumption <= 0) {
      dailyCalciumConsumption = settings.initialCalciumConsumption;
    }
  } else {
    Serial.print("Letzte Calcium-Messung: ");
    Serial.println(formatDateTime(lastCaTimestamp));
  }

  // Eingangswerte prüfen und debuggen
  Serial.println("------ EINGANGSWERTE ------");
  Serial.print("dailyCalciumConsumption: ");
  Serial.print(dailyCalciumConsumption);
  Serial.println(" ml/Tag");
  Serial.print("settings.targetCalcium: ");
  Serial.print(settings.targetCalcium);
  Serial.println(" mg/l");
  Serial.print("settings.maxDailyChangeCalcium: ");
  Serial.print(settings.maxDailyChangeCalcium);
  Serial.println(" mg/l/Tag");

  // Umrechnungsfaktor holen und debuggen
  float caPerMl = calculateCaPerML();
  Serial.print("Umrechnungsfaktor caPerMl: ");
  Serial.print(caPerMl, 6);
  Serial.println(" mg/l/ml");

  if (caPerMl <= 0) {
    Serial.println("FEHLER: caPerMl ist 0 oder negativ! Abbruch der Berechnung.");
    return;
  }

  // Startwert bestimmen
  float startCa = getLatestValue(true);

  // WICHTIG: Wenn keine Messungen vorhanden, Target als Start verwenden
  if (startCa == 0 && lastCaTimestamp == 0) {
    startCa = settings.targetCalcium;
    Serial.println("⚠️ Keine Messwerte - verwende Target-Calcium als Startwert");
  }

  float targetCa = settings.targetCalcium;
  float totalDifference = abs(targetCa - startCa);
  bool isIncrease = (targetCa > startCa);

  Serial.print("Calcium-Startwert: ");
  Serial.print(startCa);
  Serial.println(" mg/l");
  Serial.print("Ziel-Calcium: ");
  Serial.print(targetCa);
  Serial.println(" mg/l");
  Serial.print("Differenz: ");
  Serial.print(totalDifference);
  Serial.print(" mg/l (");
  Serial.print(isIncrease ? "ERHÖHUNG" : "SENKUNG");
  Serial.println(")");

  // Wenn bereits am Ziel, nur Erhaltungsdosierung
  if (totalDifference < 1.0) {
    Serial.println("Ziel bereits erreicht, nur Erhaltungsdosierung");

    // Plan initialisieren
    memset(caDosagePlan, 0, sizeof(caDosagePlan));
    caDosagePlanSize = 0;

    // Nur Erhaltungsdosierung hinzufügen
    float maintenanceCaDosage = dailyCalciumConsumption / 12.0;
    if (maintenanceCaDosage <= 0) {
      maintenanceCaDosage = settings.initialCalciumConsumption / 12.0;
    }

    float maintenanceMgDosage = 0.0;
    if (settings.magnesiumRatio > 0) {
      maintenanceMgDosage = maintenanceCaDosage * (settings.magnesiumRatio / 100.0);
    }

    caDosagePlan[0].date = 0;  // 0 = Erhaltungsdosierung
    caDosagePlan[0].caDosage = maintenanceCaDosage;
    caDosagePlan[0].mgDosage = maintenanceMgDosage;
    caDosagePlan[0].projectedCa = startCa;
    caDosagePlan[0].isMaintenanceDose = true;
    caDosagePlanSize = 1;

    lastCaPlanCalculation = now;
    saveCaDosagePlan();
    return;
  }

  // Natürlicher Verbrauch berechnen
  float naturalConsumptionMgL = (dailyCalciumConsumption / 12.0) * caPerMl;
  if (naturalConsumptionMgL <= 0) {
    naturalConsumptionMgL = (settings.initialCalciumConsumption / 12.0) * caPerMl;
  }

  Serial.print("Natürlicher Verbrauch: ");
  Serial.print(naturalConsumptionMgL);
  Serial.println(" mg/l pro Intervall");

  // Eingestelltes Maximum aus den Settings
  float settingsMaxChangePerInterval = settings.maxDailyChangeCalcium / 12.0;

  // Physikalisches Maximum bei Senkung
  float physicalMaxDecreasePerInterval = naturalConsumptionMgL;

  // Tatsächlich verwendetes Maximum bestimmen
  float maxChangePerInterval;

  if (isIncrease) {
    // ERHÖHUNG: Unbegrenzt nach oben (nur durch Einstellungen begrenzt)
    maxChangePerInterval = settingsMaxChangePerInterval;
    Serial.print("Erhöhung: Nutze Einstellungs-Maximum = ");
    Serial.print(maxChangePerInterval);
    Serial.println(" mg/l pro Intervall");

  } else {
    // SENKUNG: Durch natürlichen Verbrauch begrenzt
    maxChangePerInterval = min(settingsMaxChangePerInterval, physicalMaxDecreasePerInterval);

    Serial.print("Senkung: Einstellungs-Maximum = ");
    Serial.print(settingsMaxChangePerInterval);
    Serial.print(" mg/l, Physikalisches Maximum = ");
    Serial.print(physicalMaxDecreasePerInterval);
    Serial.print(" mg/l, Verwendet = ");
    Serial.print(maxChangePerInterval);
    Serial.println(" mg/l pro Intervall");

    // Warnung bei Begrenzung durch physikalisches Limit
    if (physicalMaxDecreasePerInterval < settingsMaxChangePerInterval) {
      Serial.println("⚠️  WARNUNG: Calcium-Senkung durch natürlichen Verbrauch begrenzt!");
      Serial.print("Eingestelltes Maximum: ");
      Serial.print(settingsMaxChangePerInterval * 12);
      Serial.println(" mg/l/Tag");
      Serial.print("Tatsächlich möglich: ");
      Serial.print(physicalMaxDecreasePerInterval * 12);
      Serial.println(" mg/l/Tag");
      Serial.println("Grund: Natürlicher Verbrauch begrenzt die Senkungsgeschwindigkeit");

      // Warnung an WebSocket senden
      DynamicJsonDocument doc(512);
      doc["type"] = "warning";
      doc["message"] = "Calcium-Senkung durch natürlichen Verbrauch begrenzt";
      doc["settingsMax"] = settingsMaxChangePerInterval * 12;
      doc["physicalMax"] = physicalMaxDecreasePerInterval * 12;
      doc["parameter"] = "Calcium";
      String json;
      serializeJson(doc, json);
      ws.textAll(json);
    }
  }

  // KORRIGIERTE Calcium-Berechnung (mit Debug)
  Serial.println("------ CALCIUM-BERECHNUNGSSCHRITTE ------");
  Serial.print("1. Erste Berechnung: totalDifference=");
  Serial.print(totalDifference, 2);
  Serial.print(" mg/l, maxChangePerInterval=");
  Serial.print(maxChangePerInterval, 3);
  Serial.println(" mg/l");

  int numDosages;
  float changePerDosage;

  if (!isIncrease) {
    // CALCIUM-SENKUNG: Korrekte Berechnung mit vollständigen Verbrauchs-Einheiten
    float fullUnits = floor(totalDifference / maxChangePerInterval);
    float remainder = totalDifference - (fullUnits * maxChangePerInterval);

    Serial.print("2. CALCIUM-SENKUNG - Vollständige Einheiten: ");
    Serial.print(fullUnits);
    Serial.print(" × ");
    Serial.print(maxChangePerInterval, 3);
    Serial.print(" = ");
    Serial.print(fullUnits * maxChangePerInterval, 2);
    Serial.println(" mg/l");

    Serial.print("   Rest: ");
    Serial.print(remainder, 3);
    Serial.println(" mg/l");

    if (remainder > 0.01) {  // 0.01 mg/l Toleranz für Calcium
      numDosages = (int)fullUnits + 1;
      Serial.print("   Benötigte Dosierungen: ");
      Serial.print((int)fullUnits);
      Serial.println(" vollständige + 1 partielle");
    } else {
      numDosages = (int)fullUnits;
      Serial.print("   Benötigte Dosierungen: ");
      Serial.print((int)fullUnits);
      Serial.println(" vollständige");
    }

    // Keine gleichmäßige Aufteilung - verwende natürliche Einheiten
    changePerDosage = maxChangePerInterval;

  } else {
    // CALCIUM-ERHÖHUNG: Bisherige bewährte Logik beibehalten
    numDosages = ceil(totalDifference / maxChangePerInterval);
    changePerDosage = totalDifference / numDosages;

    Serial.print("2. CALCIUM-ERHÖHUNG - Initial numDosages = ceil(");
    Serial.print(totalDifference, 2);
    Serial.print(" / ");
    Serial.print(maxChangePerInterval, 3);
    Serial.print(") = ");
    Serial.println(numDosages);

    Serial.print("4. CALCIUM-ERHÖHUNG - Normale Aufteilung: ");
    Serial.print(totalDifference, 2);
    Serial.print(" / ");
    Serial.print(numDosages);
    Serial.print(" = ");
    Serial.print(changePerDosage, 3);
    Serial.println(" mg/l pro Dosierung");
  }

  Serial.println("------ FINALE CALCIUM-WERTE ------");
  Serial.print("Finale Dosierungen: ");
  Serial.println(numDosages);
  Serial.print("Finale Änderung pro Dosierung: ");
  Serial.print(changePerDosage, 3);
  Serial.println(" mg/l");
  Serial.print("Geschätzte Dauer: ");
  Serial.print((float)numDosages * 2.0 / 24.0, 1);
  Serial.println(" Tage");

  // Berechne die nächste Dosierzeit für Calcium
  time_t nextCaDosingTime = 0;
  int caHour = getHourFromTime(lastCaTimestamp);
  int caMinute = Europe_Berlin.minute(lastCaTimestamp);

  // Finde die nächste gerade Stunde für Dosierung
  int caNextDosingHour;
  if (caHour % 2 == 0) {
    if (caMinute < 10) {
      caNextDosingHour = caHour;
    } else {
      caNextDosingHour = (caHour + 2) % 24;
    }
  } else {
    caNextDosingHour = (caHour + 1) % 24;
  }

  int caDaysToAdd = 0;
  if (caNextDosingHour < caHour) {
    caDaysToAdd = 1;
  }

  nextCaDosingTime = getTimeForHourOnDay(lastCaTimestamp, caDaysToAdd, caNextDosingHour);

  // Plan initialisieren
  memset(caDosagePlan, 0, sizeof(caDosagePlan));
  caDosagePlanSize = 0;

  // Dosierungen erstellen
  int caPlanEntries = 0;
  time_t currentDosageTime = nextCaDosingTime;

  if (isIncrease) {
    // ERHÖHUNG: Anpassung zur Erhaltung hinzufügen
    float adjustmentPerDosage = changePerDosage / caPerMl;
    float maintenanceDosage = dailyCalciumConsumption / 12.0;
    if (maintenanceDosage <= 0) {
      maintenanceDosage = settings.initialCalciumConsumption / 12.0;
    }

    Serial.println("------ ERHÖHUNGS-DOSIERUNGEN ------");
    Serial.print("Anpassung pro Dosierung: ");
    Serial.print(adjustmentPerDosage);
    Serial.println(" ml");
    Serial.print("Erhaltung pro Dosierung: ");
    Serial.print(maintenanceDosage);
    Serial.println(" ml");

    for (int i = 0; i < numDosages && caPlanEntries < MAX_CA_PLAN_ENTRIES - 1; i++) {
      float totalCaDosage = adjustmentPerDosage + maintenanceDosage;
      float totalMgDosage = 0.0;

      if (settings.magnesiumRatio > 0) {
        totalMgDosage = totalCaDosage * (settings.magnesiumRatio / 100.0);
      }

      float projectedCa = startCa + (i + 1) * changePerDosage;

      caDosagePlan[caPlanEntries].date = currentDosageTime;
      caDosagePlan[caPlanEntries].caDosage = totalCaDosage;
      caDosagePlan[caPlanEntries].mgDosage = totalMgDosage;
      caDosagePlan[caPlanEntries].projectedCa = projectedCa;
      caDosagePlan[caPlanEntries].isMaintenanceDose = false;
      caPlanEntries++;

      currentDosageTime += (2 * 3600);  // Nächste Dosierung in 2 Stunden
    }

  } else {
    // CALCIUM-SENKUNG: KORRIGIERTE Erhaltung reduzieren oder stoppen
    float maintenanceDosage = dailyCalciumConsumption / 12.0;
    if (maintenanceDosage <= 0) {
      maintenanceDosage = settings.initialCalciumConsumption / 12.0;
    }

    Serial.println("------ CALCIUM-SENKUNGS-DOSIERUNGEN (KORRIGIERT) ------");
    Serial.print("Normale Erhaltung: ");
    Serial.print(maintenanceDosage, 2);
    Serial.println(" ml");
    Serial.print("Natürlicher Verbrauch: ");
    Serial.print(naturalConsumptionMgL, 3);
    Serial.println(" mg/l pro Intervall");

    float remainingReduction = totalDifference;

    for (int i = 0; i < numDosages && caPlanEntries < MAX_CA_PLAN_ENTRIES - 1; i++) {
      Serial.print("=== CALCIUM-DOSIERUNG ");
      Serial.print(i + 1);
      Serial.println(" ===");

      // Berechne die tatsächlich benötigte Reduktion für diese Dosierung
      float requiredReduction = min(remainingReduction, maxChangePerInterval);
      float finalCaDosage = 0;

      Serial.print("Verbleibende Calcium-Gesamtreduktion: ");
      Serial.print(remainingReduction, 3);
      Serial.println(" mg/l");
      Serial.print("Calcium-Reduktion diese Dosierung: ");
      Serial.print(requiredReduction, 3);
      Serial.println(" mg/l");

      if (requiredReduction <= naturalConsumptionMgL) {
        Serial.print("✅ Calcium-Reduktion möglich (");
        Serial.print(requiredReduction, 3);
        Serial.print(" <= ");
        Serial.print(naturalConsumptionMgL, 3);
        Serial.println(")");

        float dosageReduction = requiredReduction / caPerMl;
        Serial.print("Dosierungs-Reduktion: ");
        Serial.print(requiredReduction, 3);
        Serial.print(" mg/l / ");
        Serial.print(caPerMl, 6);
        Serial.print(" = ");
        Serial.print(dosageReduction, 2);
        Serial.println(" ml weniger");

        finalCaDosage = maintenanceDosage - dosageReduction;
        finalCaDosage = max(0.0f, finalCaDosage);

        Serial.print("Finale Ca-Dosierung: ");
        Serial.print(maintenanceDosage, 2);
        Serial.print(" - ");
        Serial.print(dosageReduction, 2);
        Serial.print(" = ");
        Serial.print(finalCaDosage, 2);
        Serial.println(" ml");

        // Aktualisiere verbleibende Reduktion
        remainingReduction -= requiredReduction;
      } else {
        Serial.print("❌ Calcium-Reduktion NICHT möglich (");
        Serial.print(requiredReduction, 3);
        Serial.print(" > ");
        Serial.print(naturalConsumptionMgL, 3);
        Serial.println(") - stoppe Dosierung");
        finalCaDosage = 0;

        // Bei unmöglicher Reduktion: verbleibende Reduktion um natürlichen Verbrauch reduzieren
        remainingReduction -= min(remainingReduction, naturalConsumptionMgL);
      }

      float finalMgDosage = 0.0;
      if (settings.magnesiumRatio > 0) {
        finalMgDosage = finalCaDosage * (settings.magnesiumRatio / 100.0);
      }

      // KORRIGIERTE Calcium-Prognose-Berechnung
      float totalReductionSoFar = totalDifference - remainingReduction;
      float projectedCa = startCa - totalReductionSoFar;
      projectedCa = max(projectedCa, targetCa);

      Serial.print("Calcium-Prognose: ");
      Serial.print(startCa, 1);
      Serial.print(" - ");
      Serial.print(totalReductionSoFar, 3);
      Serial.print(" = ");
      Serial.print(projectedCa, 1);
      Serial.println(" mg/l");

      caDosagePlan[caPlanEntries].date = currentDosageTime;
      caDosagePlan[caPlanEntries].caDosage = finalCaDosage;
      caDosagePlan[caPlanEntries].mgDosage = finalMgDosage;
      caDosagePlan[caPlanEntries].projectedCa = projectedCa;
      caDosagePlan[caPlanEntries].isMaintenanceDose = false;
      caPlanEntries++;

      currentDosageTime += (2 * 3600);  // Nächste Dosierung in 2 Stunden

      // Beende Schleife wenn Ziel erreicht
      if (remainingReduction <= 0.01) {  // 0.01 mg/l Toleranz
        Serial.println("Calcium-Zielwert erreicht - beende Dosierungsplanung");
        break;
      }
    }
  }

  // Erhaltungsdosierung hinzufügen
  float maintenanceCaDosage = dailyCalciumConsumption / 12.0;
  if (maintenanceCaDosage <= 0) {
    maintenanceCaDosage = settings.initialCalciumConsumption / 12.0;
  }

  float maintenanceMgDosage = 0.0;
  if (settings.magnesiumRatio > 0) {
    maintenanceMgDosage = maintenanceCaDosage * (settings.magnesiumRatio / 100.0);
  }

  caDosagePlan[caPlanEntries].date = 0;  // 0 = Erhaltungsdosierung (unendlich)
  caDosagePlan[caPlanEntries].caDosage = maintenanceCaDosage;
  caDosagePlan[caPlanEntries].mgDosage = maintenanceMgDosage;
  caDosagePlan[caPlanEntries].projectedCa = targetCa;  // Stabilisiert bei Zielwert
  caDosagePlan[caPlanEntries].isMaintenanceDose = true;
  caPlanEntries++;

  // Aktualisiere die Anzahl der tatsächlichen Einträge
  caDosagePlanSize = caPlanEntries;

  // Aktualisiere den Zeitstempel der letzten Planberechnung
  lastCaPlanCalculation = now;

  // Plan speichern
  saveCaDosagePlan();

  Serial.println("------ CALCIUM-ZUSAMMENFASSUNG ------");
  Serial.print("Anpassungsdosierungen: ");
  Serial.println(caPlanEntries - 1);
  Serial.print("Gesamteinträge im Plan: ");
  Serial.println(caDosagePlanSize);
  Serial.print("Geschätzte Dauer: ");
  Serial.print((float)(caPlanEntries - 1) * 2.0 / 24.0, 1);
  Serial.println(" Tage");
  Serial.println("===================================");
}

bool saveKHDosagePlan() {
  Serial.println("Speichere KH-Dosierplan in LittleFS...");
  File file = LittleFS.open(KH_DOSAGE_PLAN_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("FEHLER: Öffnen von KH-Dosierplan zum Schreiben fehlgeschlagen!");
    return false;
  }

  // KRITISCHER FIX: Heap-Allokation statt Stack (16KB würde Stack sprengen!)
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation für JSON-Dokument fehlgeschlagen!");
    file.close();
    return false;
  }

  // Speichere die tatsächliche Größe des Plans
  (*doc)["size"] = khDosagePlanSize;

  // Speichere Zeitstempel der letzten Berechnung
  (*doc)["lastCalculation"] = lastKHPlanCalculation;

  // Erstelle Array für Plan-Einträge
  JsonArray planArray = doc->createNestedArray("entries");

  int entriesWritten = 0;
  for (int i = 0; i < khDosagePlanSize; ++i) {
    JsonObject entry = planArray.createNestedObject();
    entry["date"] = khDosagePlan[i].date;
    entry["dosage"] = khDosagePlan[i].dosage;
    entry["projectedValue"] = khDosagePlan[i].projectedValue;
    entry["isNightDosage"] = khDosagePlan[i].isNightDosage;
    entry["isMaintenanceDose"] = khDosagePlan[i].isMaintenanceDose;
    entriesWritten++;
  }

  size_t bytesWritten = serializeJson(*doc, file);
  file.close();
  delete doc;  // Speicher freigeben

  if (bytesWritten > 0) {
    Serial.print("KH-Dosierplan erfolgreich gespeichert (");
    Serial.print(bytesWritten);
    Serial.print(" Bytes, ");
    Serial.print(entriesWritten);
    Serial.println(" Einträge).");
    return true;
  } else {
    Serial.println("FEHLER: Schreiben des KH-Dosierplans in Datei fehlgeschlagen!");
    return false;
  }
}

// Calcium-Dosierplan als JSON speichern
bool saveCaDosagePlan() {
  Serial.println("Speichere Calcium-Dosierplan in LittleFS...");
  File file = LittleFS.open(CA_DOSAGE_PLAN_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("FEHLER: Öffnen von Calcium-Dosierplan zum Schreiben fehlgeschlagen!");
    return false;
  }

  // KRITISCHER FIX: Heap-Allokation statt Stack (16KB würde Stack sprengen!)
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation für JSON-Dokument fehlgeschlagen!");
    file.close();
    return false;
  }

  // Speichere die tatsächliche Größe des Plans
  (*doc)["size"] = caDosagePlanSize;

  // Speichere Zeitstempel der letzten Berechnung
  (*doc)["lastCalculation"] = lastCaPlanCalculation;

  // Erstelle Array für Plan-Einträge
  JsonArray planArray = doc->createNestedArray("entries");

  int entriesWritten = 0;
  for (int i = 0; i < caDosagePlanSize; ++i) {
    JsonObject entry = planArray.createNestedObject();
    entry["date"] = caDosagePlan[i].date;
    entry["caDosage"] = caDosagePlan[i].caDosage;
    entry["mgDosage"] = caDosagePlan[i].mgDosage;
    entry["projectedCa"] = caDosagePlan[i].projectedCa;
    entry["isMaintenanceDose"] = caDosagePlan[i].isMaintenanceDose;
    entriesWritten++;
  }

  size_t bytesWritten = serializeJson(*doc, file);
  file.close();
  delete doc;  // Speicher freigeben

  if (bytesWritten > 0) {
    Serial.print("Calcium-Dosierplan erfolgreich gespeichert (");
    Serial.print(bytesWritten);
    Serial.print(" Bytes, ");
    Serial.print(entriesWritten);
    Serial.println(" Einträge).");
    return true;
  } else {
    Serial.println("FEHLER: Schreiben des Calcium-Dosierplans in Datei fehlgeschlagen!");
    return false;
  }
}

// KH-Dosierplan aus JSON laden
bool loadKHDosagePlan() {
  Serial.println("Lade KH-Dosierplan aus LittleFS...");

  // Zuerst den Plan leeren
  memset(khDosagePlan, 0, sizeof(khDosagePlan));
  khDosagePlanSize = 0;
  lastKHPlanCalculation = 0;

  if (!LittleFS.exists(KH_DOSAGE_PLAN_FILE)) {
    Serial.println("KH-Dosierplan-Datei nicht gefunden. Überspringe Laden.");
    return false;
  }

  File file = LittleFS.open(KH_DOSAGE_PLAN_FILE, FILE_READ);
  if (!file) {
    Serial.println("FEHLER: Öffnen von KH-Dosierplan zum Lesen fehlgeschlagen!");
    return false;
  }

  // KRITISCHER FIX: Heap-Allokation statt Stack (16KB würde Stack sprengen!)
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation für JSON-Dokument fehlgeschlagen!");
    file.close();
    return false;
  }

  DeserializationError error = deserializeJson(*doc, file);
  file.close();

  if (error) {
    Serial.print("FEHLER: Deserialisieren des KH-Dosierplans fehlgeschlagen: ");
    Serial.println(error.c_str());
    delete doc;
    return false;
  }

  // Lade den Zeitstempel der letzten Berechnung
  if (doc->containsKey("lastCalculation")) {
    lastKHPlanCalculation = (*doc)["lastCalculation"];
  }

  // Lade die Plangröße
  if (doc->containsKey("size")) {
    khDosagePlanSize = (*doc)["size"];

    // Sicherheitsüberprüfung
    if (khDosagePlanSize > MAX_KH_PLAN_ENTRIES) {
      khDosagePlanSize = MAX_KH_PLAN_ENTRIES;
    }
  }

  // Lade Planinhalte
  if (doc->containsKey("entries") && (*doc)["entries"].is<JsonArray>()) {
    JsonArray planArray = (*doc)["entries"];
    int entriesRead = 0;

    for (int i = 0; i < khDosagePlanSize && i < planArray.size() && i < MAX_KH_PLAN_ENTRIES; ++i) {
      JsonObject entry = planArray[i];

      // Prüfe, ob die Keys existieren
      if (entry.containsKey("date") && entry.containsKey("dosage") && entry.containsKey("projectedValue")) {
        khDosagePlan[i].date = entry["date"];
        khDosagePlan[i].dosage = entry["dosage"];
        khDosagePlan[i].projectedValue = entry["projectedValue"];
        khDosagePlan[i].isNightDosage = entry["isNightDosage"] | false;
        khDosagePlan[i].isMaintenanceDose = entry["isMaintenanceDose"] | false;
        entriesRead++;
      }
    }

    // Aktualisiere die tatsächliche Anzahl gelesener Einträge
    khDosagePlanSize = entriesRead;

    Serial.print("KH-Dosierplan erfolgreich geladen (");
    Serial.print(entriesRead);
    Serial.println(" Einträge).");
  } else {
    Serial.println("Keine gültigen Planeinträge gefunden!");
    delete doc;  // Speicher freigeben
    return false;
  }

  delete doc;  // Speicher freigeben
  return true;
}

// Calcium-Dosierplan aus JSON laden
bool loadCaDosagePlan() {
  Serial.println("Lade Calcium-Dosierplan aus LittleFS...");

  // Zuerst den Plan leeren
  memset(caDosagePlan, 0, sizeof(caDosagePlan));
  caDosagePlanSize = 0;
  lastCaPlanCalculation = 0;

  if (!LittleFS.exists(CA_DOSAGE_PLAN_FILE)) {
    Serial.println("Calcium-Dosierplan-Datei nicht gefunden. Überspringe Laden.");
    return false;
  }

  File file = LittleFS.open(CA_DOSAGE_PLAN_FILE, FILE_READ);
  if (!file) {
    Serial.println("FEHLER: Öffnen von Calcium-Dosierplan zum Lesen fehlgeschlagen!");
    return false;
  }

  // KRITISCHER FIX: Heap-Allokation statt Stack (16KB würde Stack sprengen!)
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation für JSON-Dokument fehlgeschlagen!");
    file.close();
    return false;
  }

  DeserializationError error = deserializeJson(*doc, file);
  file.close();

  if (error) {
    Serial.print("FEHLER: Deserialisieren des Calcium-Dosierplans fehlgeschlagen: ");
    Serial.println(error.c_str());
    delete doc;
    return false;
  }

  // Lade den Zeitstempel der letzten Berechnung
  if (doc->containsKey("lastCalculation")) {
    lastCaPlanCalculation = (*doc)["lastCalculation"];
  }

  // Lade die Plangröße
  if (doc->containsKey("size")) {
    caDosagePlanSize = (*doc)["size"];

    // Sicherheitsüberprüfung
    if (caDosagePlanSize > MAX_CA_PLAN_ENTRIES) {
      caDosagePlanSize = MAX_CA_PLAN_ENTRIES;
    }
  }

  // Lade Planinhalte
  if (doc->containsKey("entries") && (*doc)["entries"].is<JsonArray>()) {
    JsonArray planArray = (*doc)["entries"];
    int entriesRead = 0;

    for (int i = 0; i < caDosagePlanSize && i < planArray.size() && i < MAX_CA_PLAN_ENTRIES; ++i) {
      JsonObject entry = planArray[i];

      // Prüfe, ob die Keys existieren
      if (entry.containsKey("date") && entry.containsKey("caDosage") && entry.containsKey("projectedCa")) {
        caDosagePlan[i].date = entry["date"];
        caDosagePlan[i].caDosage = entry["caDosage"];
        caDosagePlan[i].mgDosage = entry["mgDosage"] | 0.0f;  // Falls nicht vorhanden, 0
        caDosagePlan[i].projectedCa = entry["projectedCa"];
        caDosagePlan[i].isMaintenanceDose = entry["isMaintenanceDose"] | false;
        entriesRead++;
      }
    }

    // Aktualisiere die tatsächliche Anzahl gelesener Einträge
    caDosagePlanSize = entriesRead;

    Serial.print("Calcium-Dosierplan erfolgreich geladen (");
    Serial.print(entriesRead);
    Serial.println(" Einträge).");
  } else {
    Serial.println("Keine gültigen Planeinträge gefunden!");
    delete doc;
    return false;
  }

  delete doc;  // Speicher freigeben
  return true;
}

// KH-Dosierplan als JSON
String getAllKhDosagePlanJson() {
  // KRITISCHER FIX: Heap-Allokation statt Stack (16KB würde Stack sprengen!)
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation für KH-Plan JSON fehlgeschlagen!");
    return "{\"error\":\"Memory allocation failed\"}";
  }

  // Autodosierstatus hinzufügen
  (*doc)["autoDosing"] = settings.autoDosing;
  JsonArray planArray = doc->createNestedArray("khDosagePlan");

  // Einträge nur hinzufügen, wenn automatische Dosierung aktiviert ist
  if (settings.autoDosing) {
    // Alle Einträge aus dem KH-Dosierplan hinzufügen
    for (int i = 0; i < khDosagePlanSize; i++) {
      JsonObject entry = planArray.createNestedObject();
      entry["date"] = khDosagePlan[i].date;

      if (khDosagePlan[i].date == 0) {
        entry["formattedDate"] = "∞";  // Unendlichzeichen für Erhaltungsdosierung
      } else {
        entry["formattedDate"] = formatDateTime(khDosagePlan[i].date);
      }

      // Füge Überprüfungen hinzu, um sicherzustellen, dass keine ungültigen Werte gesendet werden
      float dosage = khDosagePlan[i].dosage;
      if (isnan(dosage) || isinf(dosage)) {
        dosage = 0.0;
      }
      entry["dosage"] = dosage;

      float projectedValue = khDosagePlan[i].projectedValue;
      if (isnan(projectedValue) || isinf(projectedValue)) {
        projectedValue = 0.0;
      }
      entry["projectedValue"] = projectedValue;

      entry["type"] = khDosagePlan[i].isMaintenanceDose ? "Erhaltung" : "Ausgleich";
    }
  }

  String jsonString;
  serializeJson(*doc, jsonString);
  delete doc;  // Speicher freigeben
  return jsonString;
}

// Calcium-Dosierplan als JSON
String getAllCaDosagePlanJson() {
  // KRITISCHER FIX: Heap-Allokation statt Stack (16KB würde Stack sprengen!)
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation für Ca-Plan JSON fehlgeschlagen!");
    return "{\"error\":\"Memory allocation failed\"}";
  }

  // Autodosierstatus hinzufügen
  (*doc)["autoDosing"] = settings.autoDosing;
  JsonArray planArray = doc->createNestedArray("caDosagePlan");

  // Einträge nur hinzufügen, wenn automatische Dosierung aktiviert ist
  if (settings.autoDosing) {
    // Alle Einträge aus dem Ca-Dosierplan hinzufügen
    for (int i = 0; i < caDosagePlanSize; i++) {
      JsonObject entry = planArray.createNestedObject();
      entry["date"] = caDosagePlan[i].date;

      if (caDosagePlan[i].date == 0) {
        entry["formattedDate"] = "∞";  // Unendlichzeichen für Erhaltungsdosierung
      } else {
        entry["formattedDate"] = formatDateTime(caDosagePlan[i].date);
      }

      // Füge Überprüfungen hinzu, um sicherzustellen, dass keine ungültigen Werte gesendet werden
      // Ein Floatwert ist ungültig, wenn er NaN oder Infinity ist

      float caDosage = caDosagePlan[i].caDosage;
      if (isnan(caDosage) || isinf(caDosage)) {
        caDosage = 0.0;
      }
      entry["caDosage"] = caDosage;

      float mgDosage = caDosagePlan[i].mgDosage;
      if (isnan(mgDosage) || isinf(mgDosage)) {
        mgDosage = 0.0;
      }
      entry["mgDosage"] = mgDosage;

      float projectedCa = caDosagePlan[i].projectedCa;
      if (isnan(projectedCa) || isinf(projectedCa)) {
        projectedCa = 0.0;
      }
      entry["projectedCa"] = projectedCa;

      entry["type"] = caDosagePlan[i].isMaintenanceDose ? "Erhaltung" : "Ausgleich";
    }
  }

  String jsonString;
  serializeJson(*doc, jsonString);
  delete doc;  // Speicher freigeben
  return jsonString;
}

// Beide Dosierpläne an alle Clients senden
void broadcastDosagePlans() {
  if (ws.count() == 0) return;  // Nichts senden wenn keine Clients verbunden

  // Große JSON-Payloads (~16KB each) nacheinander senden mit yield() dazwischen
  String khPlanJson = getAllKhDosagePlanJson();
  yield();  // Watchdog Reset vor großem ws.textAll()
  ws.textAll(khPlanJson);
  khPlanJson = "";  // Speicher sofort freigeben

  yield();  // Watchdog Reset zwischen den beiden Broadcasts

  String caPlanJson = getAllCaDosagePlanJson();
  yield();  // Watchdog Reset vor großem ws.textAll()
  ws.textAll(caPlanJson);
  caPlanJson = "";  // Speicher sofort freigeben

  Serial.println("Alle Dosierpläne an alle Clients gesendet");
}

// KH-Dosierplan an einen spezifischen Client senden
void sendKHDosagePlanToClient(AsyncWebSocketClient *client) {
  String khPlanJson = getAllKhDosagePlanJson();  // Bestehende Funktion verwenden
  client->text(khPlanJson);
  Serial.print("KH-Dosierplan an Client ");
  Serial.print(client->id());
  Serial.print(" gesendet (");
  Serial.print(khPlanJson.length());
  Serial.println(" Bytes)");
}

// Calcium-Dosierplan an einen spezifischen Client senden
void sendCaDosagePlanToClient(AsyncWebSocketClient *client) {
  String caPlanJson = getAllCaDosagePlanJson();  // Bestehende Funktion verwenden
  client->text(caPlanJson);
  Serial.print("Ca-Dosierplan an Client ");
  Serial.print(client->id());
  Serial.print(" gesendet (");
  Serial.print(caPlanJson.length());
  Serial.println(" Bytes)");
}

// Beide Dosierpläne an einen spezifischen Client senden
// KEIN delay() verwenden — blockiert den async TCP-Stack!
void sendAllDosagePlansToClient(AsyncWebSocketClient *client) {
  sendKHDosagePlanToClient(client);
  yield();  // Watchdog Reset zwischen zwei großen JSON-Payloads (~16KB each)
  sendCaDosagePlanToClient(client);
}

// Neuer Einstiegspunkt für Dosierplan-Aktualisierungen
void updateDosagePlans(bool updateKH, bool updateCa) {
  // Berechnungszeit messen
  unsigned long startTime = millis();

  // KH-Plan aktualisieren, wenn angefordert
  if (updateKH) {
    calculateKHDosagePlan();
    yield();  // Watchdog Reset nach schwerer Berechnung
  }

  // Calcium-Plan aktualisieren, wenn angefordert
  if (updateCa) {
    calculateCaDosagePlan();
    yield();  // Watchdog Reset nach schwerer Berechnung
  }

  // Pläne an UI senden mit der verbesserten Funktion
  broadcastDosagePlans();

  // Berechnungszeit ausgeben
  unsigned long endTime = millis();
  Serial.print("Dosierplan-Aktualisierung in ");
  Serial.print(endTime - startTime);
  Serial.println(" ms abgeschlossen");
}

// Diese Funktion wird nach dem Speichern einer neuen Messung aufgerufen
void handleNewMeasurement(bool isCalcium) {
  Serial.print("Neue ");
  Serial.print(isCalcium ? "Calcium" : "KH");
  Serial.println("-Messung erkannt");

  // Verbrauch neu berechnen
  calculateConsumption();

  // Nur den betroffenen Plan aktualisieren
  updateDosagePlans(!isCalcium, isCalcium);
}

// Hauptfunktion zur Prüfung und Ausführung der automatischen Dosierung
void checkAndPerformAutoDosing() {
  // Variable-Deklarationen für den gesamten Funktionsbereich
  bool needKhDosage = false;
  bool needCaDosage = false;
  bool needMgDosage = false;

  // Nur wenn automatische Dosierung aktiviert ist
  if (!settings.autoDosing) {
    Serial.println("Automatische Dosierung ist deaktiviert");
    return;
  }

  // Wenn bereits eine Dosierungssequenz aktiv ist, nichts tun
  if (dosageSequenceActive) {
    Serial.println("Dosierungssequenz bereits aktiv, warte auf Abschluss");
    logDosingEvent("CHECK_SKIPPED", "Sequenz bereits aktiv");
    return;
  }

  logDosingEvent("CHECK_START", "Prüfe automatische Dosierung");

  // Aktuelle Zeit holen
  time_t now = getCurrentTime();

  // Aktuelle Stunde und Minute bestimmen
  int currentHour = Europe_Berlin.hour(now);
  int currentMinute = Europe_Berlin.minute(now);

  Serial.print("Prüfe automatische Dosierung. Aktuelle Zeit: ");
  Serial.print(formatDateTime(now));
  Serial.print(", Stunde: ");
  Serial.print(currentHour);
  Serial.print(", Minute: ");
  Serial.println(currentMinute);

  // Prüfen, ob wir im Dosierzeitfenster sind (10-15 Minuten nach gerader Stunde)
  if (currentHour % 2 != 0 || (currentMinute < 10 || currentMinute > 15)) {
    Serial.println("Nicht im Dosierzeitfenster (gerade Stunde, Minute 10-15)");
    return;
  }

  // Prüfen, ob die KH- und Ca-Dosierpläne aktuell sind
  bool khPlanValid = (khDosagePlanSize > 0);
  bool caPlanValid = (caDosagePlanSize > 0);

  // Fehlende Pläne neu berechnen
  if (!khPlanValid) {
    Serial.println("KH-Dosierplan fehlt oder ist leer, berechne neu");
    calculateKHDosagePlan();
    khPlanValid = (khDosagePlanSize > 0);
  }

  if (!caPlanValid) {
    Serial.println("Ca-Dosierplan fehlt oder ist leer, berechne neu");
    calculateCaDosagePlan();
    caPlanValid = (caDosagePlanSize > 0);
  }

  // KORRIGIERT: Prüfe spezifisch für die aktuelle Stunde, ob eine Ausgleichsdosis geplant ist
  bool khInMaintenancePhase = true;
  bool caInMaintenancePhase = true;

  // Aktuelle Stunde für Vergleich
  time_t hourStart = getTimeForHourOnDay(now, 0, currentHour);
  time_t hourEnd = hourStart + 3600 - 1;  // Ende der Stunde

  // KH-Plan prüfen: Gibt es eine Ausgleichsdosis für die aktuelle Stunde?
  for (int i = 0; i < khDosagePlanSize; i++) {
    if (khDosagePlan[i].date >= hourStart && khDosagePlan[i].date <= hourEnd && isSameDay(khDosagePlan[i].date, now)) {
      khInMaintenancePhase = false;  // Es gibt eine spezifische Ausgleichsdosis für diese Stunde
      break;
    }
  }

  // Ca-Plan separat prüfen: Gibt es eine Ausgleichsdosis für die aktuelle Stunde?
  for (int i = 0; i < caDosagePlanSize; i++) {
    if (caDosagePlan[i].date >= hourStart && caDosagePlan[i].date <= hourEnd && isSameDay(caDosagePlan[i].date, now)) {
      caInMaintenancePhase = false;  // Es gibt eine spezifische Ausgleichsdosis für diese Stunde
      break;
    }
  }

  Serial.print("KH in Erhaltungsphase: ");
  Serial.println(khInMaintenancePhase ? "Ja" : "Nein");
  Serial.print("Ca in Erhaltungsphase: ");
  Serial.println(caInMaintenancePhase ? "Ja" : "Nein");

  // KH und Ca/Mg Dosierentscheidungen getrennt verarbeiten

  // 1. KH-Dosierungsentscheidung
  float khDosage = 0.0;
  bool khIsNightDosage = false;
  bool khDosageFound = false;

  if (khInMaintenancePhase) {
    // ERHALTUNGSPHASE für KH: Prüfen, ob in der aktuellen Stunde bereits dosiert wurde

    // Aktuellen Tag und Stunde ermitteln
    time_t currentDay = getStartOfDay(now);
    int currentHour = getHourFromTime(now);

    // Prüfe via RAM-Cache, ob bereits eine KH-Dosierung (Tag oder Nacht) in dieser Stunde erfolgt ist
    bool alreadyDosedThisHour = false;

    // KH-Tag: Letzter Zeitstempel aus Cache prüfen
    time_t lastKHDay = lastDosageTimeCache[DOSAGE_TYPE_KH_DAY];
    if (lastKHDay != 0 && getStartOfDay(lastKHDay) == currentDay && getHourFromTime(lastKHDay) == currentHour) {
      alreadyDosedThisHour = true;
      Serial.print("Bereits KH-Tag-Dosierung in Stunde ");
      Serial.print(currentHour);
      Serial.println(" vorhanden (Cache)");
    }

    // KH-Nacht: Letzter Zeitstempel aus Cache prüfen
    if (!alreadyDosedThisHour) {
      time_t lastKHNight = lastDosageTimeCache[DOSAGE_TYPE_KH_NIGHT];
      if (lastKHNight != 0 && getStartOfDay(lastKHNight) == currentDay && getHourFromTime(lastKHNight) == currentHour) {
        alreadyDosedThisHour = true;
        Serial.print("Bereits KH-Nacht-Dosierung in Stunde ");
        Serial.print(currentHour);
        Serial.println(" vorhanden (Cache)");
      }
    }

    // Dosierung nur, wenn in dieser Stunde noch nicht dosiert wurde
    needKhDosage = !alreadyDosedThisHour;

    if (!needKhDosage) {
      Serial.println("KH-Erhaltungsdosierung übersprungen (bereits in dieser Stunde dosiert)");
    } else {
      Serial.println("KH-Erhaltungsdosierung nötig (noch nicht in dieser Stunde dosiert)");
      // Erhaltungsdosierung aus dem Plan holen
      for (int i = 0; i < khDosagePlanSize; i++) {
        if (khDosagePlan[i].isMaintenanceDose) {
          khDosage = khDosagePlan[i].dosage;

          // Schwankungskompensation anwenden (nur auf Erhaltungsdosierung)
          if (dosingFactorsEnabled) {
            int interval = currentHour / 2;
            float factor = dosingFactors[interval];
            khDosage *= factor;
            Serial.printf("KH-Schwankungskompensation: Intervall %d, Faktor %.3f\n", interval, factor);
          }

          // Tag/Nacht bestimmen
          if (settings.usePhBasedKHDosing) {
            // pH-basierte KH-Dosierung
            float currentPhValue = getLatestPHValue();
            khIsNightDosage = (currentPhValue < settings.phThresholdForKHNight);
          } else {
            // Zeitbasierte KH-Dosierung
            if (settings.khNightStart < settings.khNightEnd) {
              khIsNightDosage = (currentHour >= settings.khNightStart && currentHour < settings.khNightEnd);
            } else {
              khIsNightDosage = (currentHour >= settings.khNightStart || currentHour < settings.khNightEnd);
            }
          }

          khDosageFound = true;
          Serial.print("KH-Erhaltungsdosierung: ");
          Serial.print(khDosage);
          Serial.print(" ml, Nacht: ");
          Serial.println(khIsNightDosage ? "Ja" : "Nein");
          break;
        }
      }
    }
  } else {
    // AUSGLEICHSPHASE für KH: Prüfen, ob in dieser Stunde bereits dosiert wurde
    int lastDosingHour = -1;
    if (settings.lastAutoDosage > 0) {
      // Stunde der letzten Dosierung ermitteln
      lastDosingHour = Europe_Berlin.hour(settings.lastAutoDosage);

      // Prüfen, ob es heute war und KH betraf
      if (isSameDay(settings.lastAutoDosage, now)) {
        // Hier müssten wir eigentlich prüfen, ob tatsächlich KH dosiert wurde,
        // vereinfacht nehmen wir an, dass die Stunde für alle KH-Dosierungen gilt
        Serial.print("Letzte KH-Dosierung in Stunde: ");
        Serial.print(lastDosingHour);
        Serial.print(" am ");
        Serial.println(formatDateTime(settings.lastAutoDosage));
      }
    }

    // Wenn in dieser Stunde bereits dosiert wurde, KH-Dosierung überspringen
    if (lastDosingHour == currentHour && isSameDay(settings.lastAutoDosage, now)) {
      Serial.println("KH wurde in dieser Stunde bereits dosiert, überspringe");
    } else {
      // Ausgleichsdosierung aus dem Plan für die aktuelle Stunde suchen
      time_t hourStart = getTimeForHourOnDay(now, 0, currentHour);
      time_t hourEnd = hourStart + 3600 - 1;  // Ende der Stunde

      for (int i = 0; i < khDosagePlanSize; i++) {
        // Wenn ein Eintrag mit Datum im aktuellen Stundenfenster gefunden wird
        if (khDosagePlan[i].date >= hourStart && khDosagePlan[i].date <= hourEnd && isSameDay(khDosagePlan[i].date, now)) {
          khDosage = khDosagePlan[i].dosage;
          khDosageFound = (khDosage > 0);  // Nur dosieren wenn > 0

          // pH-Entscheidung IMMER zur Laufzeit treffen (NICHT aus Plan verwenden):
          if (settings.usePhBasedKHDosing) {
            float currentPhValue = getLatestPHValue();
            khIsNightDosage = (currentPhValue < settings.phThresholdForKHNight);

            Serial.print("LIVE pH-Entscheidung für geplante Dosierung: pH=");
            Serial.print(currentPhValue, 2);
            Serial.print(", Schwellwert=");
            Serial.print(settings.phThresholdForKHNight, 1);
            Serial.print(" → ");
            Serial.println(khIsNightDosage ? "KH-Nacht" : "KH-Tag");
          } else {
            // Zeitbasierte Entscheidung
            if (settings.khNightStart < settings.khNightEnd) {
              khIsNightDosage = (currentHour >= settings.khNightStart && currentHour < settings.khNightEnd);
            } else {
              khIsNightDosage = (currentHour >= settings.khNightStart || currentHour < settings.khNightEnd);
            }
          }

          Serial.print("KH-Ausgleichsdosierung aus Plan gefunden: ");
          Serial.print(khDosage);
          Serial.print(" ml, Nacht: ");
          Serial.print(khIsNightDosage ? "Ja" : "Nein");
          if (khDosage == 0) {
            Serial.println(" (0 ml = keine Dosierung)");
          } else {
            Serial.println();
          }
          break;
        }
      }

      // Kein Fallback mehr - wenn kein Plan oder 0 ml, dann wird nicht dosiert
      if (!khDosageFound) {
        Serial.println("Keine KH-Dosierung geplant für diese Stunde");
      }
    }
  }

  // 2. Ca/Mg-Dosierungsentscheidung
  float caDosage = 0.0;
  float mgDosage = 0.0;
  bool caDosageFound = false;

  if (caInMaintenancePhase) {
    // ERHALTUNGSPHASE für Ca/Mg: Prüfen, ob in der aktuellen Stunde bereits dosiert wurde

    // Aktuellen Tag und Stunde ermitteln
    time_t currentDay = getStartOfDay(now);
    int currentHour = getHourFromTime(now);

    // Prüfe via RAM-Cache, ob bereits eine Calcium-Dosierung in dieser Stunde erfolgt ist
    bool alreadyDosedCaThisHour = false;

    time_t lastCa = lastDosageTimeCache[DOSAGE_TYPE_CALCIUM];
    if (lastCa != 0 && getStartOfDay(lastCa) == currentDay && getHourFromTime(lastCa) == currentHour) {
      alreadyDosedCaThisHour = true;
      Serial.print("Bereits Calcium-Dosierung in Stunde ");
      Serial.print(currentHour);
      Serial.println(" vorhanden (Cache)");
    }

    // Prüfe via RAM-Cache, ob bereits eine Magnesium-Dosierung in dieser Stunde erfolgt ist
    bool alreadyDosedMgThisHour = false;

    time_t lastMg = lastDosageTimeCache[DOSAGE_TYPE_MAGNESIUM];
    if (lastMg != 0 && getStartOfDay(lastMg) == currentDay && getHourFromTime(lastMg) == currentHour) {
      alreadyDosedMgThisHour = true;
      Serial.print("Bereits Magnesium-Dosierung in Stunde ");
      Serial.print(currentHour);
      Serial.println(" vorhanden (Cache)");
    }

    // Dosierung nur, wenn in dieser Stunde noch nicht dosiert wurde
    needCaDosage = !alreadyDosedCaThisHour;
    needMgDosage = !alreadyDosedMgThisHour;

    if (!needCaDosage) {
      Serial.println("Calcium-Erhaltungsdosierung übersprungen (bereits in dieser Stunde dosiert)");
    } else {
      Serial.println("Calcium-Erhaltungsdosierung nötig (noch nicht in dieser Stunde dosiert)");
    }

    if (!needMgDosage) {
      Serial.println("Magnesium-Erhaltungsdosierung übersprungen (bereits in dieser Stunde dosiert)");
    } else {
      Serial.println("Magnesium-Erhaltungsdosierung nötig (noch nicht in dieser Stunde dosiert)");
    }

    // Ca/Mg-Erhaltungsdosierung aus dem Plan holen, falls nötig
    for (int i = 0; i < caDosagePlanSize; i++) {
      if (caDosagePlan[i].isMaintenanceDose) {
        // Schwankungskompensation-Faktor für dieses Intervall
        float intervalFactor = 1.0;
        if (dosingFactorsEnabled) {
          int interval = currentHour / 2;
          intervalFactor = dosingFactors[interval];
        }

        // Calcium-Dosierung
        if (needCaDosage) {
          caDosage = caDosagePlan[i].caDosage * intervalFactor;
          Serial.print("Ca-Erhaltungsdosierung: ");
          Serial.print(caDosage);
          Serial.print(" ml");
          if (intervalFactor != 1.0) {
            Serial.printf(" (Faktor: %.3f)", intervalFactor);
          }
          Serial.println();
        } else {
          Serial.println("Ca-Erhaltungsdosierung übersprungen (letzte Dosierung weniger als 115 Minuten her)");
        }

        // Magnesium-Dosierung
        if (needMgDosage) {
          mgDosage = caDosagePlan[i].mgDosage * intervalFactor;
          Serial.print("Mg-Erhaltungsdosierung: ");
          Serial.print(mgDosage);
          Serial.print(" ml");
          if (intervalFactor != 1.0) {
            Serial.printf(" (Faktor: %.3f)", intervalFactor);
          }
          Serial.println();
        } else {
          Serial.println("Mg-Erhaltungsdosierung übersprungen (letzte Dosierung weniger als 115 Minuten her)");
        }

        caDosageFound = true;
        break;
      }
    }
  } else {
    // AUSGLEICHSPHASE für Ca/Mg: Prüfen, ob in dieser Stunde bereits dosiert wurde
    int lastDosingHour = -1;
    if (settings.lastAutoDosage > 0) {
      // Stunde der letzten Dosierung ermitteln
      lastDosingHour = Europe_Berlin.hour(settings.lastAutoDosage);

      // Prüfen, ob es heute war und Ca/Mg betraf
      if (isSameDay(settings.lastAutoDosage, now)) {
        Serial.print("Letzte Ca/Mg-Dosierung in Stunde: ");
        Serial.print(lastDosingHour);
        Serial.print(" am ");
        Serial.println(formatDateTime(settings.lastAutoDosage));
      }
    }

    // Wenn in dieser Stunde bereits dosiert wurde, Ca/Mg-Dosierung überspringen
    if (lastDosingHour == currentHour && isSameDay(settings.lastAutoDosage, now)) {
      Serial.println("Ca/Mg wurde in dieser Stunde bereits dosiert, überspringe");
    } else {
      // Ausgleichsdosierung aus dem Plan für die aktuelle Stunde suchen
      time_t hourStart = getTimeForHourOnDay(now, 0, currentHour);
      time_t hourEnd = hourStart + 3600 - 1;  // Ende der Stunde

      for (int i = 0; i < caDosagePlanSize; i++) {
        // Wenn ein Eintrag mit Datum im aktuellen Stundenfenster gefunden wird
        if (caDosagePlan[i].date >= hourStart && caDosagePlan[i].date <= hourEnd && isSameDay(caDosagePlan[i].date, now)) {
          caDosage = caDosagePlan[i].caDosage;
          mgDosage = caDosagePlan[i].mgDosage;
          caDosageFound = (caDosage > 0 || mgDosage > 0);  // Nur dosieren wenn > 0

          Serial.print("Ca/Mg-Ausgleichsdosierung aus Plan gefunden: ");
          Serial.print(caDosage);
          Serial.print(" ml Ca, ");
          Serial.print(mgDosage);
          Serial.print(" ml Mg");
          if (caDosage == 0 && mgDosage == 0) {
            Serial.println(" (0 ml = keine Dosierung)");
          } else {
            Serial.println();
          }
          break;
        }
      }

      // Kein Fallback mehr - wenn kein Plan oder 0 ml, dann wird nicht dosiert
      if (!caDosageFound) {
        Serial.println("Keine Ca/Mg-Dosierung geplant für diese Stunde");
      }
    }
  }

  // Dosierungssequenz starten, wenn mindestens eine Dosierung geplant ist
  bool anyDosagePlanned = (khDosageFound && khDosage > 0) || (caDosageFound && (caDosage > 0 || mgDosage > 0));

  if (!anyDosagePlanned) {
    Serial.println("Keine Dosierungen geplant, nichts zu tun");
    return;
  }

  // Dosierungssequenz initialisieren
  dosageSequenceActive = true;
  pendingDosages = 0;
  currentDosageHour = currentHour;
  currentDosageTimestamp = now;

  Serial.print("Starte Dosierungssequenz für Stunde ");
  Serial.print(currentDosageHour);
  Serial.print(" um ");
  Serial.println(formatDateTime(now));

  logDosingEvent("SEQ_START", "Stunde=" + String(currentDosageHour) + " pending=0 active=true");

  // Schwankungskompensation-Faktor für Dosierhistorie bestimmen
  float khFactorForHistory = 1.0;
  float caFactorForHistory = 1.0;
  if (dosingFactorsEnabled && khInMaintenancePhase) {
    int interval = currentHour / 2;
    khFactorForHistory = dosingFactors[interval];
  }
  if (dosingFactorsEnabled && caInMaintenancePhase) {
    int interval = currentHour / 2;
    caFactorForHistory = dosingFactors[interval];
  }

  // Jetzt die Dosierungen starten, wenn sie gefunden wurden
  if (khDosageFound && khDosage > 0) {
    // Wähle die passende KH-Pumpe basierend auf Tag/Nacht-Status
    int khPumpIndex = khIsNightDosage ? 3 : 2;  // 3=KH-Nacht, 2=KH-Tag

    // Wenn Nacht-Dosierung, Menge halbieren (da doppelt konzentriert)
    if (khIsNightDosage) {
      khDosage = khDosage * 0.5;
    }

    Serial.print("KH-Dosierung eingeplant: ");
    Serial.print(khDosage);
    Serial.print(" ml mit Pumpe ");
    Serial.print(khPumpIndex);
    Serial.print(" (");
    Serial.print(pumps[khPumpIndex].name);
    Serial.println(")");

    pendingDosages++;
    currentDosageFactor = khFactorForHistory;
    logDosingEvent("DISPENSE_KH", "Pumpe=" + String(khPumpIndex) + " ml=" + String(khDosage, 2) + " Faktor=" + String(khFactorForHistory, 3) + " pending=" + String(pendingDosages));
    dispensePump(khPumpIndex, khDosage, true);
  }

  if (caDosageFound && caDosage > 0) {
    Serial.print("Calcium-Dosierung eingeplant: ");
    Serial.println(caDosage);
    pendingDosages++;
    currentDosageFactor = caFactorForHistory;
    logDosingEvent("DISPENSE_CA", "Pumpe=0 ml=" + String(caDosage, 2) + " Faktor=" + String(caFactorForHistory, 3) + " pending=" + String(pendingDosages));
    dispensePump(0, caDosage, true);  // Calcium ist Pumpe 0
  }

  if (caDosageFound && mgDosage > 0) {
    Serial.print("Magnesium-Dosierung eingeplant: ");
    Serial.println(mgDosage);
    pendingDosages++;
    currentDosageFactor = caFactorForHistory;
    logDosingEvent("DISPENSE_MG", "Pumpe=1 ml=" + String(mgDosage, 2) + " Faktor=" + String(caFactorForHistory, 3) + " pending=" + String(pendingDosages));
    dispensePump(1, mgDosage, true);  // Magnesium ist Pumpe 1
  }

  // Wenn keine Dosierungen eingeplant wurden, Sequenz als beendet markieren
  if (pendingDosages == 0) {
    Serial.println("Keine Dosierungen eingeplant, markiere Stunde als erledigt");

    // Markiere die Dosierungssequenz als beendet
    dosageSequenceActive = false;

    // Setze trotzdem den Zeitstempel für diese Stunde, um sie als erledigt zu markieren
    settings.lastAutoDosage = currentDosageTimestamp;
    saveSettingsToJson();
  } else {
    Serial.print("Dosierungssequenz gestartet mit ");
    Serial.print(pendingDosages);
    Serial.println(" geplanten Dosierungen");
  }
}

// Aktuelle Zeit an alle Clients senden
void broadcastCurrentTime() {
  // Zeit IMMER senden (von RTC oder Time Library)
  // Funktioniert mit UND ohne RTC!

  time_t currentTime = getCurrentTime();

  DynamicJsonDocument doc(256);
  doc["type"] = "timeUpdate";
  doc["timestamp"] = currentTime;
  doc["formattedTime"] = formatDateTime(currentTime);
  doc["timeInitialized"] = timeInitialized;
  doc["timeSource"] = rtcInitialized ? "RTC" : "TimeLib";  // Debug-Info für Frontend
  doc["timeOffset"] = timeOffset;

  String jsonString;
  serializeJson(doc, jsonString);
  ws.textAll(jsonString);
}

// KH-Dosierplan als JSON - immer vollständig
String getFullKHDosagePlanJson() {
  // KRITISCHER FIX: Heap allocation instead of stack
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation fehlgeschlagen!");
    return "{\"error\":\"Memory allocation failed\"}";
  }
  JsonArray planArray = doc->createNestedArray("khDosagePlan");

  // Alle Einträge hinzufügen
  for (int i = 0; i < khDosagePlanSize; i++) {
    JsonObject entry = planArray.createNestedObject();
    entry["date"] = khDosagePlan[i].date;
    entry["formattedDate"] = khDosagePlan[i].date == 0 ? "∞" : formatDateTime(khDosagePlan[i].date);
    entry["dosage"] = khDosagePlan[i].dosage;
    entry["projectedValue"] = khDosagePlan[i].projectedValue;
    entry["isNightDosage"] = khDosagePlan[i].isNightDosage;
    entry["type"] = khDosagePlan[i].isMaintenanceDose ? "Erhaltung" : "Ausgleich";
  }

  String jsonString;
  serializeJson(*doc, jsonString);
  delete doc;  // CRITICAL: Free memory
  return jsonString;
}

// Calcium-Dosierplan als JSON - immer vollständig
String getFullCaDosagePlanJson() {
  // KRITISCHER FIX: Heap allocation instead of stack
  PsramJsonDocument* doc = new PsramJsonDocument(16384);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation fehlgeschlagen!");
    return "{\"error\":\"Memory allocation failed\"}";
  }
  JsonArray planArray = doc->createNestedArray("caDosagePlan");

  // Alle Einträge hinzufügen
  for (int i = 0; i < caDosagePlanSize; i++) {
    JsonObject entry = planArray.createNestedObject();
    entry["date"] = caDosagePlan[i].date;
    entry["formattedDate"] = caDosagePlan[i].date == 0 ? "∞" : formatDateTime(caDosagePlan[i].date);
    entry["caDosage"] = caDosagePlan[i].caDosage;
    entry["mgDosage"] = caDosagePlan[i].mgDosage;
    entry["projectedCa"] = caDosagePlan[i].projectedCa;
    entry["type"] = caDosagePlan[i].isMaintenanceDose ? "Erhaltung" : "Ausgleich";
  }

  String jsonString;
  serializeJson(*doc, jsonString);
  delete doc;  // CRITICAL: Free memory
  return jsonString;
}

float calculateDaysRemaining(int containerIndex) {
  int dosageType;
  switch (containerIndex) {
    case 0: dosageType = DOSAGE_TYPE_CALCIUM; break;
    case 1: dosageType = DOSAGE_TYPE_MAGNESIUM; break;
    case 2: dosageType = DOSAGE_TYPE_KH_DAY; break;
    case 3: dosageType = DOSAGE_TYPE_KH_NIGHT; break;
    default: return 999;
  }

  // Lade alle verfügbaren Dosierungen (für Robustheit)
  int dosageCount;
  Dosage* dosages = getAllDosages(dosageType, dosageCount);

  if (dosageCount == 0) {
    psram_delete_array(dosages);
    return 999;  // Keine Daten vorhanden
  }

  // ✅ VERBESSERUNG: Nur die letzten 3 Tage für Durchschnitt verwenden
  time_t now = getCurrentTime();
  time_t threeDaysAgo = now - (3L * 24L * 60L * 60L);  // 3 Tage zurück (Overflow-sicher)

  float recentVolume = 0;   // Volumen der letzten 3 Tage
  time_t oldestRecent = 0;  // Älteste relevante Dosierung
  time_t newestRecent = 0;  // Neueste relevante Dosierung
  int recentCount = 0;      // Anzahl relevanter Dosierungen

  // ✅ Filtere nur Dosierungen der letzten 3 Tage
  for (int j = 0; j < dosageCount; j++) {
    if (dosages[j].timestamp >= threeDaysAgo) {
      recentVolume += dosages[j].amount;
      recentCount++;

      if (oldestRecent == 0 || dosages[j].timestamp < oldestRecent) {
        oldestRecent = dosages[j].timestamp;
      }
      if (newestRecent == 0 || dosages[j].timestamp > newestRecent) {
        newestRecent = dosages[j].timestamp;
      }
    }
  }

  psram_delete_array(dosages);

  // Fallback: Wenn keine Dosierungen in den letzten 3 Tagen
  if (recentCount == 0) {
    Serial.print("Keine Dosierungen in den letzten 3 Tagen für ");
    Serial.println(pumps[containerIndex].name);
    return 999;  // Unbegrenzt
  }

  // ✅ Berechne tatsächlichen Zeitraum der relevanten Dosierungen
  float actualDays = (float)((double)(newestRecent - oldestRecent) / (24.0 * 60.0 * 60.0));

  // Sanitize: Schutz gegen Infinity/NaN/negative Werte (z.B. durch korrupte Timestamps)
  if (isnan(actualDays) || isinf(actualDays) || actualDays < 0 || actualDays > 365) {
    Serial.printf("WARNUNG: actualDays ungültig (oldest=%ld, newest=%ld), setze auf 1.0\n",
                  (long)oldestRecent, (long)newestRecent);
    actualDays = 1.0;
  }

  // Mindestens 0.5 Tage für die Berechnung (realistischer als 1 Tag)
  if (actualDays < 0.5) {
    actualDays = 0.5;
  }

  // ✅ Aktueller täglicher Verbrauch (nur letzten 3 Tage)
  float dailyUsage = recentVolume / actualDays;
  float currentLevel = settings.containerLevel[containerIndex];

  // Sanitize: Schutz gegen Infinity/NaN
  if (isnan(dailyUsage) || isinf(dailyUsage) || dailyUsage < 0) {
    dailyUsage = 0;
  }

  // Debug-Ausgabe
  Serial.print("Container ");
  Serial.print(pumps[containerIndex].name);
  Serial.print(": ");
  Serial.print(recentCount);
  Serial.print(" Dosierungen in ");
  Serial.print(actualDays, 1);
  Serial.print(" Tagen = ");
  Serial.print(dailyUsage, 2);
  Serial.print(" ml/Tag, Füllstand: ");
  Serial.print(currentLevel, 0);
  Serial.print(" ml → ");

  float daysRemaining = (dailyUsage > 0) ? (currentLevel / dailyUsage) : 999;

  // Sanitize: Schutz gegen Infinity/NaN
  if (isnan(daysRemaining) || isinf(daysRemaining) || daysRemaining < 0) {
    daysRemaining = 999;
  }

  Serial.print(daysRemaining, 1);
  Serial.println(" Tage");

  return daysRemaining;
}

// Aktuelle Einstellungen als JSON-String
String getSystemSettingsJson() {
  DynamicJsonDocument doc(2048);  // Increased size to accommodate container data

  doc["settings"]["aquariumVolume"] = settings.aquariumVolume;
  doc["settings"]["targetKH"] = settings.targetKH;
  doc["settings"]["targetCalcium"] = settings.targetCalcium;
  doc["settings"]["historyCount"] = settings.historyCount;
  doc["settings"]["maxDailyChangeKH"] = settings.maxDailyChangeKH;
  doc["settings"]["maxDailyChangeCalcium"] = settings.maxDailyChangeCalcium;
  doc["settings"]["autoDosing"] = settings.autoDosing;
  doc["settings"]["magnesiumRatio"] = settings.magnesiumRatio;
  doc["settings"]["khNightStart"] = settings.khNightStart;
  doc["settings"]["khNightEnd"] = settings.khNightEnd;
  doc["settings"]["initialKHConsumption"] = settings.initialKHConsumption;
  doc["settings"]["initialCalciumConsumption"] = settings.initialCalciumConsumption;
  doc["settings"]["autoUpdateInitialRates"] = settings.autoUpdateInitialRates;
  doc["settings"]["usePhBasedKHDosing"] = settings.usePhBasedKHDosing;
  doc["settings"]["phThresholdForKHNight"] = settings.phThresholdForKHNight;
  doc["settings"]["timeOffset"] = timeOffset;

  // Aktuelle Verbrauchswerte hinzufügen
  doc["settings"]["dailyKHConsumption"] = dailyKHConsumption;
  doc["settings"]["dailyCalciumConsumption"] = dailyCalciumConsumption;

  // Aktuelle Werte hinzufügen
  doc["settings"]["currentKH"] = getLatestValue(false);
  doc["settings"]["currentCalcium"] = getLatestValue(true);

  // Nächste Dosierung hinzufügen (aus dem Dosierplan)
  // Suche nach der nächsten geplanten Dosierung oder Erhaltungsdosierung
  if (khDosagePlanSize > 0) {
    // Finde die erste relevante Dosierung (entweder zukünftig oder Erhaltung)
    time_t now = getCurrentTime();
    float nextKHDosage = 0.0;
    float nextKHProjected = 0.0;
    bool foundKH = false;

    for (int i = 0; i < khDosagePlanSize; i++) {
      // Entweder zukünftige Dosierung oder Erhaltungsdosierung
      if (khDosagePlan[i].date >= now || khDosagePlan[i].isMaintenanceDose) {
        nextKHDosage = khDosagePlan[i].dosage;
        nextKHProjected = khDosagePlan[i].projectedValue;
        foundKH = true;
        break;
      }
    }

    if (foundKH) {
      doc["settings"]["nextKHDosage"] = nextKHDosage;
      doc["settings"]["nextKHProjected"] = nextKHProjected;
    }
  }

  if (caDosagePlanSize > 0) {
    // Finde die erste relevante Dosierung (entweder zukünftig oder Erhaltung)
    time_t now = getCurrentTime();
    float nextCaDosage = 0.0;
    float nextCaProjected = 0.0;
    bool foundCa = false;

    for (int i = 0; i < caDosagePlanSize; i++) {
      // Entweder zukünftige Dosierung oder Erhaltungsdosierung
      if (caDosagePlan[i].date >= now || caDosagePlan[i].isMaintenanceDose) {
        nextCaDosage = caDosagePlan[i].caDosage;
        nextCaProjected = caDosagePlan[i].projectedCa;
        foundCa = true;
        break;
      }
    }

    if (foundCa) {
      doc["settings"]["nextCalciumDosage"] = nextCaDosage;
      doc["settings"]["nextCalciumProjected"] = nextCaProjected;
    }
  }

  // Letzte automatische Dosierung formatiert hinzufügen
  if (settings.lastAutoDosage > 0) {
    doc["settings"]["lastAutoDosageFormatted"] = formatDateTime(settings.lastAutoDosage);
  }

  // Anti-Tropf-Einstellungen hinzufügen
  doc["settings"]["enableAntiDrip"] = settings.enableAntiDrip;
  doc["settings"]["antiDripML"] = settings.antiDripML;
  doc["settings"]["antiDripSpeedML"] = settings.antiDripSpeedML;

  // Add container data
  JsonArray containersArray = doc["settings"].createNestedArray("containers");
  for (int i = 0; i < 4; i++) {
    JsonObject container = containersArray.createNestedObject();
    container["id"] = i;
    container["name"] = pumps[i].name;
    container["capacity"] = settings.containerCapacity[i];
    container["level"] = settings.containerLevel[i];
    float pct = (settings.containerCapacity[i] > 0) ? (settings.containerLevel[i] / settings.containerCapacity[i] * 100) : 0;
    container["percentage"] = (isnan(pct) || isinf(pct)) ? 0 : pct;

    // Nutze die Hilfsfunktion zur Berechnung der verbleibenden Tage
    float days = calculateDaysRemaining(i);
    container["daysRemaining"] = (isnan(days) || isinf(days)) ? 999 : days;

    // Konsistentes Datums- und Zeitformat verwenden
    container["lastRefill"] = formatDateTime(settings.lastContainerRefill[i]);
  }

  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

// EEPROM komplett zurücksetzen
void resetSettings() {
  // Pumpen-Konfigurationen zurücksetzen
  for (int i = 0; i < 4; i++) {
    // Standardwerte setzen
    pumps[i].mlPerStep = 0.0;
    pumps[i].speedML = DEFAULT_SPEED_ML;
    pumps[i].accelerationML = DEFAULT_ACCELERATION_ML;
    pumps[i].lastCalibrationDate = 0;
  }

  // Pumpen-Konfiguration speichern
  savePumpsToJson();

  // Systemeinstellungen zurücksetzen
  settings.aquariumVolume = DEFAULT_AQUARIUM_VOLUME;
  settings.targetKH = DEFAULT_TARGET_KH;
  settings.targetCalcium = DEFAULT_TARGET_CALCIUM;
  settings.historyCount = DEFAULT_HISTORY_COUNT;
  settings.autoDosing = false;
  settings.maxDailyChangeKH = DEFAULT_MAX_DAILY_CHANGE_KH;
  settings.maxDailyChangeCalcium = DEFAULT_MAX_DAILY_CHANGE_CALCIUM;
  settings.lastAutoDosage = 0;
  settings.magnesiumRatio = DEFAULT_MAGNESIUM_RATIO;
  settings.khNightStart = DEFAULT_KH_NIGHT_START;
  settings.khNightEnd = DEFAULT_KH_NIGHT_END;
  settings.initialKHConsumption = DEFAULT_INITIAL_KH_CONSUMPTION;
  settings.initialCalciumConsumption = DEFAULT_INITIAL_CALCIUM_CONSUMPTION;
  settings.autoUpdateInitialRates = true;
  settings.usePhBasedKHDosing = false;
  settings.phThresholdForKHNight = DEFAULT_PH_THRESHOLD;
  settings.enableAntiDrip = DEFAULT_ENABLE_ANTI_DRIP;
  settings.antiDripML = DEFAULT_ANTI_DRIP_ML;
  settings.antiDripSpeedML = DEFAULT_ANTI_DRIP_SPEED_ML;

  // Reset container data
  for (int i = 0; i < 4; i++) {
    settings.containerCapacity[i] = DEFAULT_CONTAINER_CAPACITY;
    settings.containerLevel[i] = DEFAULT_CONTAINER_CAPACITY;  // Start with full containers
    settings.lastContainerRefill[i] = getCurrentTime();
  }

  // Einstellungen speichern
  saveSettingsToJson();

  // Reset pH-Kalibrierung
  phCal.voltage_pH4 = 3000.0;
  phCal.voltage_pH7 = 2500.0;
  phCal.isCalibrated = false;
  phCal.timestamp_pH4 = 0;
  phCal.timestamp_pH7 = 0;

  // pH-Kalibrierung speichern
  savePhCalibrationToJson();

  // Messungen und Dosierungen aus LittleFS löschen
  LittleFS.remove(KH_MEASUREMENTS_FILE);
  LittleFS.remove(CA_MEASUREMENTS_FILE);
  LittleFS.remove(CA_DOSAGES_FILE);
  LittleFS.remove(MG_DOSAGES_FILE);
  LittleFS.remove(KH_DAY_DOSAGES_FILE);
  LittleFS.remove(KH_NIGHT_DOSAGES_FILE);
  LittleFS.remove(PH_MEASUREMENTS_FILE);

  // RAM-Cache für Dosierungszeitstempel zurücksetzen
  for (int i = 0; i < 4; i++) {
    lastDosageTimeCache[i] = 0;
  }

  // Dosierplan zurücksetzen
  for (int i = 0; i < MAX_KH_PLAN_ENTRIES; i++) {
    khDosagePlan[i].date = 0;
    khDosagePlan[i].dosage = 0;
    khDosagePlan[i].projectedValue = 0;
    khDosagePlan[i].isNightDosage = false;
  }
  for (int i = 0; i < MAX_CA_PLAN_ENTRIES; i++) {
    caDosagePlan[i].date = 0;
    caDosagePlan[i].caDosage = 0;
    caDosagePlan[i].mgDosage = 0;
    caDosagePlan[i].projectedCa = 0;
  }

  // Arbeitsvariablen zurücksetzen
  currentCalibrationPump = -1;
  currentCalibrationSteps = 0;
  lastPumpOperation = -1;
  lastPumpAmount = 0;

  Serial.println("Alle Einstellungen und Daten wurden zurückgesetzt!");
}

// =========== BACKUP UND RESTORE FUNKTIONEN ===========

// Erstellt ein vollständiges Backup aller Daten
void testFileSystemAccess() {
  Serial.println("\n=== Testing LittleFS Access ===");

  // List all files in LittleFS
  Serial.println("Files in LittleFS:");
  File root = LittleFS.open("/");
  if (!root) {
    Serial.println("- Failed to open directory");
    return;
  }

  if (!root.isDirectory()) {
    Serial.println("- Not a directory");
    return;
  }

  File file = root.openNextFile();
  int fileCount = 0;

  while (file) {
    fileCount++;
    const char* fileName = file.name();
    size_t fileSize = file.size();

    Serial.print("- ");
    Serial.print(fileName);
    Serial.print(" (");
    Serial.print(fileSize);
    Serial.println(" bytes)");

    // For dosage files, try to read the first entry to verify format
    if (strstr(fileName, "_dosages.bin") != nullptr) {
      Serial.print("  * Checking file format for: ");
      Serial.println(fileName);

      // Try to read the first dosage entry
      if (fileSize >= sizeof(Dosage)) {
        Dosage firstDosage;
        size_t bytesRead = file.read((uint8_t*)&firstDosage, sizeof(Dosage));

        if (bytesRead == sizeof(Dosage)) {
          Serial.print("    First entry timestamp: ");
          Serial.print(formatDateTime(firstDosage.timestamp));
          Serial.print(", Amount: ");
          Serial.print(firstDosage.amount);
          Serial.print(", Type: ");
          Serial.println(getDosageTypeName(firstDosage.dosageType));
        } else {
          Serial.print("    Failed to read first entry. Read ");
          Serial.print(bytesRead);
          Serial.print(" bytes, expected ");
          Serial.println(sizeof(Dosage));
        }
      } else {
        Serial.println("    File is empty or too small.");
      }
    }

    file = root.openNextFile();
  }

  Serial.print("Total files found: ");
  Serial.println(fileCount);

  // Try to write and read a test file
  const char* testFileName = "/spiffs_test.txt";
  const char* testContent = "LittleFS test content";

  // Write test file
  File testFile = LittleFS.open(testFileName, FILE_WRITE);
  if (!testFile) {
    Serial.print("Failed to open test file for writing: ");
    Serial.println(testFileName);
    return;
  }

  size_t bytesWritten = testFile.print(testContent);
  testFile.close();

  Serial.print("Wrote ");
  Serial.print(bytesWritten);
  Serial.print(" bytes to test file: ");
  Serial.println(testFileName);

  // Read test file
  testFile = LittleFS.open(testFileName, FILE_READ);
  if (!testFile) {
    Serial.print("Failed to open test file for reading: ");
    Serial.println(testFileName);
    return;
  }

  String readContent = testFile.readString();
  testFile.close();

  Serial.print("Read from test file: '");
  Serial.print(readContent);
  Serial.println("'");

  // Verify content
  if (readContent == testContent) {
    Serial.println("Test file content matches!");
  } else {
    Serial.println("ERROR: Test file content does not match!");
  }

  // Clean up
  LittleFS.remove(testFileName);
  Serial.println("Test file removed.");

  Serial.println("=== LittleFS Test Complete ===\n");
}
// =========== KOMMUNIKATION ===========

// Pumpen-Status als JSON-String
String getPumpStatusJson() {
  // JSON-Objekt erstellen
  DynamicJsonDocument doc(1024);
  JsonArray pumpsArray = doc.createNestedArray("pumps");

  for (int i = 0; i < 4; i++) {
    JsonObject pump = pumpsArray.createNestedObject();
    pump["id"] = i;
    pump["name"] = pumps[i].name;
    pump["mlPerStep"] = pumps[i].mlPerStep;
    pump["speedML"] = pumps[i].speedML;
    pump["accelerationML"] = pumps[i].accelerationML;

    // Format the date using formatDateTime
    if (pumps[i].lastCalibrationDate > 0) {
      pump["lastCalibration"] = formatDateTime(pumps[i].lastCalibrationDate);
    } else {
      pump["lastCalibration"] = "Nie";
    }

    pump["isCalibrated"] = pumps[i].mlPerStep > 0;
  }

  doc["currentCalibrationPump"] = currentCalibrationPump;
  doc["currentCalibrationSteps"] = currentCalibrationSteps;
  doc["lastPumpOperation"] = lastPumpOperation;
  doc["lastPumpAmount"] = lastPumpAmount;

  // Aktivitätsstatus für Fortschrittsbalken
  doc["activePumpIndex"] = activePumpIndex;
  doc["targetSteps"] = targetSteps;
  doc["isCalibrationRunning"] = isCalibrationRunning;
  doc["isDispensingRunning"] = isDispensingRunning;

  // Zeit-Initialisierungsstatus hinzufügen
  doc["timeInitialized"] = timeInitialized;
  doc["currentTime"] = formatDateTime(getCurrentTime());

  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

// JSON für Webinterface - Messungen (bereits sortiert durch Append-Reihenfolge)
// 512 KB reichen für KH + Ca + AutoKH bei mehreren Jahren Retention.
// Pro Eintrag ~112 Byte ArduinoJson-Overhead; 512 KB ≈ 4500 kombinierte Einträge.
void buildAllWaterMeasurementsJson(PsramPrint& out) {
  PsramJsonDocument doc(524288);
  JsonArray khArray = doc.createNestedArray("kh");
  JsonArray calciumArray = doc.createNestedArray("calcium");
  JsonArray autoKhArray = doc.createNestedArray("autoKh");  // NEU: Automatische KH-Messungen

  // Daten kommen bereits timestamp-aufsteigend (Append-Only + Tombstones);
  // für "neueste zuerst" reicht Rückwärts-Iteration — kein Sort nötig.
  int khCount;
  Measurement* khMeasurements = getAllMeasurements(false, khCount);
  for (int i = khCount - 1; i >= 0; i--) {
    JsonObject measurement = khArray.createNestedObject();
    measurement["timestamp"] = khMeasurements[i].timestamp;
    measurement["value"] = khMeasurements[i].value;
    measurement["index"] = khMeasurements[i].index;
    measurement["date"] = formatDateTime(khMeasurements[i].timestamp);
  }
  psram_delete_array(khMeasurements);

  int caCount;
  Measurement* caMeasurements = getAllMeasurements(true, caCount);
  for (int i = caCount - 1; i >= 0; i--) {
    JsonObject measurement = calciumArray.createNestedObject();
    measurement["timestamp"] = caMeasurements[i].timestamp;
    measurement["value"] = caMeasurements[i].value;
    measurement["index"] = caMeasurements[i].index;
    measurement["date"] = formatDateTime(caMeasurements[i].timestamp);
  }
  psram_delete_array(caMeasurements);

  int autoKhCount;
  Measurement* autoKhMeasurements = getAllAutoKHMeasurements(autoKhCount);
  if (autoKhMeasurements != nullptr && autoKhCount > 0) {
    for (int i = autoKhCount - 1; i >= 0; i--) {
      JsonObject measurement = autoKhArray.createNestedObject();
      measurement["timestamp"] = autoKhMeasurements[i].timestamp;
      measurement["value"] = autoKhMeasurements[i].value;
      measurement["index"] = autoKhMeasurements[i].index;
      measurement["date"] = formatDateTime(autoKhMeasurements[i].timestamp);
    }
    psram_delete_array(autoKhMeasurements);
  }

  doc["forTable"] = true;

  // Direkt in PSRAM-Puffer serialisieren (kein String im Heap).
  serializeJson(doc, out);
}

void calculateConsumptionData(DailyConsumption* consumptionArray, int& arraySize, time_t startTime, time_t endTime) {
  arraySize = 0;

  Serial.println("=== OPTIMIERTE VERBRAUCHSDATEN-BERECHNUNG ===");
  Serial.print("Zeitraum: ");
  Serial.print(formatDateTime(startTime));
  Serial.print(" bis ");
  Serial.println(formatDateTime(endTime));

  // ✅ VERBESSERUNG 1: Weniger LittleFS-Zugriffe durch optimierte Kombinationsfunktionen
  int khMeasCount, caMeasCount;
  Measurement* khMeasurements = getAllMeasurements(false, khMeasCount);  // LittleFS-Read 1
  Measurement* caMeasurements = getAllMeasurements(true, caMeasCount);   // LittleFS-Read 2

  // ✅ VERBESSERUNG 2: Verwende optimierte getAllKHDosages() mit memcpy()
  int khDosCount, caCount;
  Dosage* khDosages = getAllKHDosages(khDosCount);                  // LittleFS-Read 3 (kombiniert)
  Dosage* caDosages = getAllDosages(DOSAGE_TYPE_CALCIUM, caCount);  // LittleFS-Read 4

  Serial.print("Geladene Daten: KH-Messungen=");
  Serial.print(khMeasCount);
  Serial.print(", Ca-Messungen=");
  Serial.print(caMeasCount);
  Serial.print(", KH-Dosierungen=");
  Serial.print(khDosCount);
  Serial.print(", Ca-Dosierungen=");
  Serial.println(caCount);

  // ✅ VERBESSERUNG 3: Sortiere Daten einmalig für bessere Performance
  sortMeasurementsByTimestamp(khMeasurements, khMeasCount);
  sortMeasurementsByTimestamp(caMeasurements, caMeasCount);
  sortDosagesByTimestamp(khDosages, khDosCount);
  sortDosagesByTimestamp(caDosages, caCount);

  // ✅ VERBESSERUNG 4: Gruppiere Dosierungen nach Tagen (O(n) statt O(n²))
  std::map<time_t, float> dailyKHDoses;
  std::map<time_t, float> dailyCaDoses;

  // Sammle KH-Dosierungen pro Tag mit korrekter KH-Nacht Behandlung
  for (int i = 0; i < khDosCount; i++) {
    time_t dayStart = getStartOfDay(khDosages[i].timestamp);

    // ✅ KORREKTE KH-NACHT BEHANDLUNG (konsistent mit calculateConsumption)
    if (khDosages[i].dosageType == DOSAGE_TYPE_KH_NIGHT) {
      dailyKHDoses[dayStart] += khDosages[i].amount * 2.0;  // KH-Nacht × 2 für KH-Äquivalent
    } else {
      dailyKHDoses[dayStart] += khDosages[i].amount;  // KH-Tag direkt
    }
  }

  // Sammle Ca-Dosierungen pro Tag
  for (int i = 0; i < caCount; i++) {
    time_t dayStart = getStartOfDay(caDosages[i].timestamp);
    dailyCaDoses[dayStart] += caDosages[i].amount;
  }

  Serial.print("Gruppierte Tage: KH=");
  Serial.print(dailyKHDoses.size());
  Serial.print(", Ca=");
  Serial.println(dailyCaDoses.size());

  // ✅ VERBESSERUNG 5: Umrechnungsfaktoren einmalig berechnen
  float khPerMl = calculateKHPerML();
  float caPerMl = calculateCaPerML();

  Serial.print("Umrechnungsfaktoren: KH=");
  Serial.print(khPerMl, 6);
  Serial.print(" dKH/ml, Ca=");
  Serial.print(caPerMl, 6);
  Serial.println(" mg/l/ml");

  // ✅ VERBESSERUNG 6: Effiziente Tag-für-Tag Berechnung
  for (time_t currentDay = startTime; currentDay < endTime && arraySize < 365; currentDay += 86400) {
    time_t dayEnd = currentDay + 86400;

    // ✅ SCHNELL: Dosierungen aus der Map holen (O(log n) statt O(n))
    float khDosedML = 0.0;
    float caDosedML = 0.0;

    if (dailyKHDoses.find(currentDay) != dailyKHDoses.end()) {
      khDosedML = dailyKHDoses[currentDay];  // Bereits als KH-Äquivalent
    }
    if (dailyCaDoses.find(currentDay) != dailyCaDoses.end()) {
      caDosedML = dailyCaDoses[currentDay];
    }

    // ✅ VERBESSERUNG 7: Optimierte Messungs-Suche (könnte weiter verbessert werden)
    float khBefore = -1, khAfter = -1;
    float caBefore = -1, caAfter = -1;

    // KH-Messungen durchsuchen (TODO: Könnte mit Binary Search optimiert werden)
    for (int i = 0; i < khMeasCount - 1; i++) {
      if (khMeasurements[i].timestamp <= currentDay && khMeasurements[i + 1].timestamp >= dayEnd) {
        khBefore = khMeasurements[i].value;
        khAfter = khMeasurements[i + 1].value;
        break;
      }
    }

    // Ca-Messungen durchsuchen
    for (int i = 0; i < caMeasCount - 1; i++) {
      if (caMeasurements[i].timestamp <= currentDay && caMeasurements[i + 1].timestamp >= dayEnd) {
        caBefore = caMeasurements[i].value;
        caAfter = caMeasurements[i + 1].value;
        break;
      }
    }

    // ✅ VERBESSERUNG 8: Verbrauch berechnen wenn genug Daten vorhanden
    consumptionArray[arraySize].date = currentDay + 43200;  // Mittag des Tages
    consumptionArray[arraySize].hasData = false;

    // KH-Verbrauch
    if (khBefore >= 0 && khAfter >= 0) {
      float khChange = khAfter - khBefore;
      float khAddedDKH = khDosedML * khPerMl;  // khDosedML ist bereits KH-Äquivalent
      consumptionArray[arraySize].khConsumption = khAddedDKH - khChange;
      consumptionArray[arraySize].hasData = true;
    } else {
      consumptionArray[arraySize].khConsumption = 0;
    }

    // Ca-Verbrauch (in mg/l pro 100L)
    if (caBefore >= 0 && caAfter >= 0) {
      float caChange = caAfter - caBefore;
      float caAddedMgL = caDosedML * caPerMl;
      float caConsumptionMgL = caAddedMgL - caChange;
      consumptionArray[arraySize].caConsumption = caConsumptionMgL * (100.0 / settings.aquariumVolume);
      consumptionArray[arraySize].hasData = true;
    } else {
      consumptionArray[arraySize].caConsumption = 0;
    }

    if (consumptionArray[arraySize].hasData) {
      arraySize++;
    }
  }

  // ✅ VERBESSERUNG 9: Vereinfachte 3-Tage-Glättung (optional, in-place)
  if (arraySize > 2) {
    // Glättung nur wenn mehr als 2 Datenpunkte vorhanden
    for (int i = 1; i < arraySize - 1; i++) {
      if (consumptionArray[i].hasData) {
        float khSum = consumptionArray[i].khConsumption;
        float caSum = consumptionArray[i].caConsumption;
        int count = 1;

        // Nachbarn einbeziehen falls verfügbar
        if (i > 0 && consumptionArray[i - 1].hasData) {
          khSum += consumptionArray[i - 1].khConsumption;
          caSum += consumptionArray[i - 1].caConsumption;
          count++;
        }
        if (i < arraySize - 1 && consumptionArray[i + 1].hasData) {
          khSum += consumptionArray[i + 1].khConsumption;
          caSum += consumptionArray[i + 1].caConsumption;
          count++;
        }

        if (count > 1) {
          consumptionArray[i].khConsumption = khSum / count;
          consumptionArray[i].caConsumption = caSum / count;
        }
      }
    }
  }

  // Cleanup - weniger Arrays als vorher!
  psram_delete_array(khMeasurements);
  psram_delete_array(caMeasurements);
  psram_delete_array(khDosages);
  psram_delete_array(caDosages);

  Serial.print("Verbrauchsdaten optimiert berechnet: ");
  Serial.print(arraySize);
  Serial.println(" Tage");
}

// ✅ OPTIMIERTE Chart-Daten für Web-Interface (Heap-Optimierung)
void buildWaterMeasurementsJson(int weeks, PsramPrint& out) {
  PsramJsonDocument doc(4096);
  JsonArray khArray = doc.createNestedArray("kh");
  JsonArray calciumArray = doc.createNestedArray("calcium");

  // Aktuelle Zeit holen
  time_t now = getCurrentTime();
  time_t cutoffTime = now - ((time_t)weeks * 7L * 24L * 60L * 60L);  // Overflow-sicher

  Serial.println("=== OPTIMIERTE getWaterMeasurementsJson ===");
  Serial.print("Zeitraum: ");
  Serial.print(weeks);
  Serial.println(" Wochen");

  // ✅ OPTIMIERUNG 1: Stream-basiertes Lesen direkt von LittleFS mit Filter
  // Statt ALLE zu laden und dann zu filtern, lesen wir direkt mit Filter

  // KH-Messungen direkt filtern beim Lesen (Prefix-Offset + Tombstones beachten)
  {
    File khFile = LittleFS.open(KH_MEASUREMENTS_FILE, FILE_READ);
    if (khFile) {
      uint32_t prefix = readPrefixOffset(KH_MEASUREMENTS_FILE);
      khFile.seek((size_t)prefix * sizeof(Measurement));
      int khCount = 0;
      while (khFile.available() >= (int)sizeof(Measurement)) {
        Measurement m;
        khFile.read((uint8_t*)&m, sizeof(Measurement));
        if (isTombstoneTs(m.timestamp)) continue;
        if (m.timestamp >= (uint32_t)cutoffTime) {
          JsonObject measurement = khArray.createNestedObject();
          measurement["timestamp"] = m.timestamp;
          measurement["value"] = m.value;
          measurement["date"] = formatDateTime(m.timestamp);
          khCount++;
        }
      }
      khFile.close();
      Serial.print("KH-Messungen gefiltert: ");
      Serial.println(khCount);
    }
  }

  // Ca-Messungen direkt filtern beim Lesen
  {
    File caFile = LittleFS.open(CA_MEASUREMENTS_FILE, FILE_READ);
    if (caFile) {
      uint32_t prefix = readPrefixOffset(CA_MEASUREMENTS_FILE);
      caFile.seek((size_t)prefix * sizeof(Measurement));
      int caCount = 0;
      while (caFile.available() >= (int)sizeof(Measurement)) {
        Measurement m;
        caFile.read((uint8_t*)&m, sizeof(Measurement));
        if (isTombstoneTs(m.timestamp)) continue;
        if (m.timestamp >= (uint32_t)cutoffTime) {
          JsonObject measurement = calciumArray.createNestedObject();
          measurement["timestamp"] = m.timestamp;
          measurement["value"] = m.value;
          measurement["date"] = formatDateTime(m.timestamp);
          caCount++;
        }
      }
      caFile.close();
      Serial.print("Ca-Messungen gefiltert: ");
      Serial.println(caCount);
    }
  }

  // Verbrauchsdaten hinzufügen
  JsonArray khConsumptionArray = doc.createNestedArray("khConsumption");
  JsonArray caConsumptionArray = doc.createNestedArray("caConsumption");

  // ✅ OPTIMIERUNG 2: Dynamisches Array statt fester 365 Tage
  int maxDays = (now - cutoffTime) / (24 * 60 * 60) + 1;
  maxDays = min(maxDays, 365);  // Sicherheits-Limit

  DailyConsumption* consumption = psram_new_array<DailyConsumption>(maxDays);
  int consumptionCount = 0;

  Serial.print("Dynamisches Array: ");
  Serial.print(maxDays);
  Serial.print(" Tage statt 365 (Ersparnis: ");
  Serial.print((365 - maxDays) * sizeof(DailyConsumption));
  Serial.println(" Bytes)");

  calculateConsumptionData(consumption, consumptionCount, cutoffTime, now);

  // Verbrauchsdaten zum JSON hinzufügen
  for (int i = 0; i < consumptionCount; i++) {
    JsonObject khCons = khConsumptionArray.createNestedObject();
    khCons["timestamp"] = consumption[i].date;
    khCons["value"] = consumption[i].khConsumption;
    khCons["date"] = formatDateTime(consumption[i].date);

    JsonObject caCons = caConsumptionArray.createNestedObject();
    caCons["timestamp"] = consumption[i].date;
    caCons["value"] = consumption[i].caConsumption;
    caCons["date"] = formatDateTime(consumption[i].date);
  }

  psram_delete_array(consumption);

  doc["forChart"] = true;

  // Direkt in PSRAM-Puffer serialisieren (kein String im Heap).
  serializeJson(doc, out);

  Serial.print("JSON Größe: ");
  Serial.print(out.size());
  Serial.println(" Bytes");
}

// Hilfsfunktion zum Hinzufügen von Dosierungen zu einem JSON-Array
void addDosagesToJsonArray(int dosageType, JsonArray& historyArray) {
  int count;
  Dosage* dosages = getAllDosages(dosageType, count);

  for (int i = 0; i < count; i++) {
    JsonObject entry = historyArray.createNestedObject();

    // Alle Dosierungen werden als physische ml angezeigt
    float displayAmount = dosages[i].amount;
    String displayName = pumps[dosages[i].pumpIndex].name;

    // Markiere manuelle Dosierungen in der Anzeige
    if (!dosages[i].isAutomatic) {
      displayName = pumps[dosages[i].pumpIndex].name + " (manuell)";
    }

    entry["timestamp"] = dosages[i].timestamp;
    entry["pumpIndex"] = dosages[i].pumpIndex;
    entry["pumpName"] = displayName;
    entry["amount"] = displayAmount;
    entry["dosageType"] = dosages[i].dosageType;
    entry["typeName"] = getDosageTypeName(dosages[i].dosageType);
    entry["isAutomatic"] = dosages[i].isAutomatic;
    entry["date"] = formatDateTime(dosages[i].timestamp);
    entry["index"] = dosages[i].index;
  }

  psram_delete_array(dosages);
}

void extractDaysFromDosages(Dosage* dosages, int count) {
  Serial.print("Extrahiere Tage aus ");
  Serial.print(count);
  Serial.println(" Dosierungen");

  for (int i = 0; i < count; i++) {
    // Vollständiges Datum und Zeit erhalten
    String fullDate = formatDateTime(dosages[i].timestamp);

    // Robusteres Parsen mit Fehlerbehandlung
    int spacePos = fullDate.indexOf(' ');
    if (spacePos <= 0) {
      Serial.print("Ungültiges Datumsformat: ");
      Serial.println(fullDate);
      continue;  // Ungültiges Format überspringen
    }

    String dateOnly = fullDate.substring(0, spacePos);

    // Debug-Info
    if (i < 5) {  // Nur die ersten 5 ausgeben, um Spam zu vermeiden
      Serial.print("Dosierung #");
      Serial.print(i);
      Serial.print(": ");
      Serial.print(fullDate);
      Serial.print(" -> extrahiert: ");
      Serial.println(dateOnly);
    }

    // Prüfen, ob der Tag bereits existiert
    bool exists = false;
    for (size_t j = 0; j < historyDays.size(); j++) {
      if (historyDays[j] == dateOnly) {
        exists = true;
        break;
      }
    }

    if (!exists && !dateOnly.isEmpty()) {
      historyDays.push_back(dateOnly);
      Serial.print("Neuer Tag hinzugefügt: ");
      Serial.println(dateOnly);
    }
  }

  // Liste aller gefundenen Tage ausgeben
  Serial.print("Insgesamt gefundene eindeutige Tage: ");
  Serial.println(historyDays.size());
  for (size_t i = 0; i < historyDays.size() && i < 10; i++) {
    Serial.print(i);
    Serial.print(": ");
    Serial.println(historyDays[i]);
  }
}

// Verbesserte Funktion: Dosierungshistorie nach Datum sortiert
String getDosageHistoryJson() {
  // KRITISCHER FIX: Heap allocation instead of stack
  PsramJsonDocument* doc = new PsramJsonDocument(8192);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation fehlgeschlagen!");
    return "{\"error\":\"Memory allocation failed\"}";
  }
  JsonArray historyArray = doc->createNestedArray("dosageHistory");

  Serial.println("Erstelle Dosierungshistorie JSON (nach Datum sortiert)");

  // Hole alle Dosierungen aus den verschiedenen Dateien
  int caCount = 0, mgCount = 0, khDayCount = 0, khNightCount = 0;
  Dosage* caDosages = getAllDosages(DOSAGE_TYPE_CALCIUM, caCount);
  Dosage* mgDosages = getAllDosages(DOSAGE_TYPE_MAGNESIUM, mgCount);
  Dosage* khDayDosages = getAllDosages(DOSAGE_TYPE_KH_DAY, khDayCount);
  Dosage* khNightDosages = getAllDosages(DOSAGE_TYPE_KH_NIGHT, khNightCount);

  Serial.printf("Dosierungen: Ca=%d, Mg=%d, KH-Tag=%d, KH-Nacht=%d\n",
                caCount, mgCount, khDayCount, khNightCount);

  // Nur beim ersten Aufruf oder wenn leer: Sammle alle Tage
  if (historyDays.empty() || !historyDaysInitialized) {
    historyDays.clear();  // Sicherstellen, dass die Liste leer ist
    historyDaysInitialized = true;
    // Extrahiere eindeutige Tage aus allen Dosierungstypen
    extractDaysFromDosages(caDosages, caCount);
    extractDaysFromDosages(mgDosages, mgCount);
    extractDaysFromDosages(khDayDosages, khDayCount);
    extractDaysFromDosages(khNightDosages, khNightCount);

    // Sortiere die Tage (neueste zuerst)
    std::sort(historyDays.begin(), historyDays.end(),
              [](const String& a, const String& b) {
                // Parse Datum im Format "D.M.YY" oder "DD.MM.YY" (flexibel)
                int firstDotA = a.indexOf('.');
                int secondDotA = a.indexOf('.', firstDotA + 1);

                int dayA = a.substring(0, firstDotA).toInt();
                int monthA = a.substring(firstDotA + 1, secondDotA).toInt();
                int yearA = a.substring(secondDotA + 1, secondDotA + 3).toInt() + 2000;

                int firstDotB = b.indexOf('.');
                int secondDotB = b.indexOf('.', firstDotB + 1);

                int dayB = b.substring(0, firstDotB).toInt();
                int monthB = b.substring(firstDotB + 1, secondDotB).toInt();
                int yearB = b.substring(secondDotB + 1, secondDotB + 3).toInt() + 2000;

                // Vergleiche Jahr, dann Monat, dann Tag (für absteigende Sortierung)
                if (yearA != yearB) return yearA > yearB;
                if (monthA != monthB) return monthA > monthB;
                return dayA > dayB;
              });
  }

  Serial.print("Eindeutige Tage gefunden: ");
  Serial.println(historyDays.size());

  // Stelle sicher, dass der Offset gültig ist
  if (currentHistoryDayOffset >= historyDays.size()) {
    currentHistoryDayOffset = 0;
    Serial.println("Offset auf 0 zurückgesetzt (ungültiger Wert)");
  }

  // Bestimme den ausgewählten Tag
  String selectedDay = "";
  if (historyDays.size() > 0) {
    selectedDay = historyDays[currentHistoryDayOffset];
    Serial.print("Ausgewählter Tag: ");
    Serial.println(selectedDay);
  } else {
    Serial.println("Keine Tage gefunden!");
  }

  // NEU: Sammle alle Dosierungen vom ausgewählten Tag in einem temporären Array
  if (!selectedDay.isEmpty()) {
    // Temporäres Array für alle Dosierungen dieses Tags
    struct TempDosage {
      time_t timestamp;
      int pumpIndex;
      String pumpName;
      float amount;
      int dosageType;
      String typeName;
      bool isAutomatic;
      float factor;
      String date;
      int index;
    };

    std::vector<TempDosage> tempDosages;

    // Sammle alle Dosierungen vom ausgewählten Tag
    auto collectDosages = [&](Dosage* dosages, int count) {
      for (int i = 0; i < count; i++) {
        String date = formatDateTime(dosages[i].timestamp);
        String dateOnly = date.substring(0, date.indexOf(' '));

        if (dateOnly == selectedDay) {
          TempDosage temp;
          temp.timestamp = dosages[i].timestamp;

          // Sicherheitscheck für gültige Pumpenindizes
          int pumpIndex = dosages[i].pumpIndex;
          if (pumpIndex < 0 || pumpIndex >= 4) {
            pumpIndex = 0;  // Standardwert bei ungültigem Index
          }
          temp.pumpIndex = pumpIndex;

          // Alle Dosierungen werden als physische ml angezeigt
          float displayAmount = dosages[i].amount;
          String displayName = pumps[pumpIndex].name;

          // Markiere manuelle Dosierungen in der Anzeige
          if (!dosages[i].isAutomatic) {
            displayName = pumps[pumpIndex].name + " (manuell)";
          }

          temp.pumpName = displayName;
          temp.amount = displayAmount;
          temp.dosageType = dosages[i].dosageType;
          temp.typeName = getDosageTypeName(dosages[i].dosageType);
          temp.isAutomatic = dosages[i].isAutomatic;
          temp.factor = dosages[i].factor;
          temp.date = date;
          temp.index = dosages[i].index;

          tempDosages.push_back(temp);
        }
      }
    };

    // Sammle von allen Dosierungstypen
    collectDosages(caDosages, caCount);
    collectDosages(mgDosages, mgCount);
    collectDosages(khDayDosages, khDayCount);
    collectDosages(khNightDosages, khNightCount);

    // NEU: Sortiere alle Dosierungen nach Timestamp (neueste zuerst)
    std::sort(tempDosages.begin(), tempDosages.end(),
              [](const TempDosage& a, const TempDosage& b) {
                return a.timestamp > b.timestamp;  // Neueste zuerst
              });

    Serial.print("Gefilterte und sortierte Dosierungen: ");
    Serial.println(tempDosages.size());

    // Füge sortierte Dosierungen zum JSON hinzu
    for (const auto& temp : tempDosages) {
      JsonObject entry = historyArray.createNestedObject();
      entry["timestamp"] = temp.timestamp;
      entry["pumpIndex"] = temp.pumpIndex;
      entry["pumpName"] = temp.pumpName;
      entry["amount"] = temp.amount;
      entry["dosageType"] = temp.dosageType;
      entry["typeName"] = temp.typeName;
      entry["isAutomatic"] = temp.isAutomatic;
      entry["factor"] = temp.factor;
      entry["date"] = temp.date;
      entry["index"] = temp.index;
    }
  }

  // Speicher freigeben
  psram_delete_array(caDosages);
  psram_delete_array(mgDosages);
  psram_delete_array(khDayDosages);
  psram_delete_array(khNightDosages);

  // Navigationsinformationen hinzufügen
  JsonObject navigation = doc->createNestedObject("navigation");
  navigation["currentDayOffset"] = currentHistoryDayOffset;
  navigation["availableDays"] = historyDays.size();

  // Tage-Liste explizit hinzufügen als Array
  JsonArray daysArray = navigation.createNestedArray("days");
  for (size_t i = 0; i < historyDays.size(); i++) {
    daysArray.add(historyDays[i]);
  }

  // Sicherstellen, dass currentHistoryDayOffset gültig ist
  if (currentHistoryDayOffset >= historyDays.size() && historyDays.size() > 0) {
    currentHistoryDayOffset = 0;
  }

  if (historyDays.size() > 0 && currentHistoryDayOffset < historyDays.size()) {
    navigation["currentDay"] = historyDays[currentHistoryDayOffset];
  } else {
    navigation["currentDay"] = "Keine Daten";
  }

  // Leeres Ergebnis vermeiden
  if (historyArray.size() == 0) {
    JsonObject emptyEntry = historyArray.createNestedObject();
    emptyEntry["timestamp"] = 0;
    emptyEntry["pumpIndex"] = -1;
    emptyEntry["pumpName"] = "Keine Daten";
    emptyEntry["amount"] = 0;
    emptyEntry["dosageType"] = -1;
    emptyEntry["typeName"] = "Keine Daten";
    emptyEntry["isAutomatic"] = false;
    emptyEntry["date"] = "Keine Daten";
    emptyEntry["index"] = -1;
  }

  // JSON-String erstellen
  String jsonString;
  serializeJson(*doc, jsonString);
  delete doc;  // CRITICAL: Free memory

  Serial.print("JSON erstellt, Größe: ");
  Serial.print(jsonString.length());
  Serial.println(" Bytes");

  return jsonString;
}

// pH-Trend-Daten für Dashboard-Chart - VERBESSERT mit Datenbegrenzung
String getPhTrendDataJson() {
  // KRITISCHER FIX: Heap allocation instead of stack
  PsramJsonDocument* doc = new PsramJsonDocument(8192);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation fehlgeschlagen!");
    return "{\"error\":\"Memory allocation failed\"}";
  }
  JsonArray phArray = doc->createNestedArray("phTrendData");

  // KONFIGURATION: Maximale Anzahl Datenpunkte für Chart
  const int MAX_PH_CHART_POINTS = 200;  // ← HIER kannst du den Wert anpassen

  // Hole alle pH-Messungen
  int phCount;
  PhMeasurement* phMeasurements = getAllPHMeasurements(phCount);

  if (phCount == 0) {
    psram_delete_array(phMeasurements);
    String jsonString;
    serializeJson(*doc, jsonString);
    delete doc;  // CRITICAL: Free memory
    return jsonString;
  }

  Serial.print("pH-Chart: ");
  Serial.print(phCount);
  Serial.print(" Messungen verfügbar, limitiere auf ");
  Serial.println(MAX_PH_CHART_POINTS);

  // Input ist bereits timestamp-aufsteigend (Append-Only + Tombstones).
  // Für "neueste zuerst" reicht In-Place-Reverse in O(n).
  for (int a = 0, b = phCount - 1; a < b; a++, b--) {
    PhMeasurement tmp = phMeasurements[a];
    phMeasurements[a] = phMeasurements[b];
    phMeasurements[b] = tmp;
  }

  // NEUE LOGIK: Begrenze auf die neuesten MAX_PH_CHART_POINTS Messungen
  int limitedCount = min(phCount, MAX_PH_CHART_POINTS);

  // Gruppiere nach Tagen und erkenne Ausreißer (nur bei begrenzten Daten)
  std::map<String, std::vector<PhMeasurement>> dayGroups;

  for (int i = 0; i < limitedCount; i++) {  // ← Verwende limitedCount statt phCount
    String dateStr = formatDateTime(phMeasurements[i].timestamp);
    String dayOnly = dateStr.substring(0, dateStr.indexOf(' '));
    dayGroups[dayOnly].push_back(phMeasurements[i]);
  }

  // Berechne Tagesmittelwerte für Ausreißer-Erkennung
  std::vector<float> dailyAverages;
  std::vector<String> dayNames;

  for (auto& pair : dayGroups) {
    float sum = 0;
    for (auto& measurement : pair.second) {
      sum += measurement.value;
    }
    float average = sum / pair.second.size();
    dailyAverages.push_back(average);
    dayNames.push_back(pair.first);
  }

  // Einfache Ausreißer-Erkennung (IQR-Methode) - nur bei genügend Daten
  std::set<String> outlierDays;
  if (dailyAverages.size() >= 4) {
    std::vector<float> sortedAverages = dailyAverages;
    std::sort(sortedAverages.begin(), sortedAverages.end());

    int q1Index = sortedAverages.size() / 4;
    int q3Index = (3 * sortedAverages.size()) / 4;
    float q1 = sortedAverages[q1Index];
    float q3 = sortedAverages[q3Index];
    float iqr = q3 - q1;
    float lowerBound = q1 - 1.5 * iqr;
    float upperBound = q3 + 1.5 * iqr;

    // Schütze die letzten 2 Tage vor Filterung
    int protectedDays = min(2, (int)dayNames.size());
    for (int i = 0; i < dailyAverages.size() - protectedDays; i++) {
      if (dailyAverages[i] < lowerBound || dailyAverages[i] > upperBound) {
        outlierDays.insert(dayNames[i]);
      }
    }
  }

  // Sammle alle Messungen in einem temporären Array mit Timestamp
  struct TempMeasurement {
    time_t timestamp;
    float value;
    String date;
    String dayOnly;
  };

  std::vector<TempMeasurement> tempMeasurements;

  // Erstelle Datenpunkte (ohne Ausreißer-Tage)
  for (auto& pair : dayGroups) {
    if (outlierDays.find(pair.first) != outlierDays.end()) {
      continue;  // Ausreißer-Tag überspringen
    }

    for (auto& measurement : pair.second) {
      TempMeasurement temp;
      temp.timestamp = measurement.timestamp;
      temp.value = measurement.value;
      temp.date = formatDateTime(measurement.timestamp);
      temp.dayOnly = pair.first;
      tempMeasurements.push_back(temp);
    }
  }

  // Sortiere alle Messungen nach Timestamp (neueste zuerst)
  std::sort(tempMeasurements.begin(), tempMeasurements.end(),
            [](const TempMeasurement& a, const TempMeasurement& b) {
              return a.timestamp > b.timestamp;  // Neueste zuerst
            });

  // ZUSÄTZLICHE BEGRENZUNG: Falls nach Ausreißer-Filterung immer noch zu viele Daten
  if (tempMeasurements.size() > MAX_PH_CHART_POINTS) {
    tempMeasurements.resize(MAX_PH_CHART_POINTS);
    Serial.print("pH-Chart: Nach Ausreißer-Filterung auf ");
    Serial.print(MAX_PH_CHART_POINTS);
    Serial.println(" Punkte begrenzt");
  }

  // Markiere die ersten 12 Einträge als recent (das sind die neuesten)
  int highlightCount = min(12, (int)tempMeasurements.size());

  // Füge alle Messungen zum JSON hinzu, jetzt in der richtigen Reihenfolge
  for (int i = 0; i < tempMeasurements.size(); i++) {
    JsonObject entry = phArray.createNestedObject();
    entry["timestamp"] = tempMeasurements[i].timestamp;
    entry["value"] = tempMeasurements[i].value;
    entry["date"] = tempMeasurements[i].date;
    entry["dayOnly"] = tempMeasurements[i].dayOnly;

    // Markiere die ersten highlightCount Einträge als recent (neueste)
    if (i < highlightCount) {
      entry["isRecent"] = true;
    } else {
      entry["isRecent"] = false;
    }
  }

  psram_delete_array(phMeasurements);

  Serial.print("pH-Chart: Finale Anzahl Datenpunkte = ");
  Serial.println(phArray.size());

  String jsonString;
  serializeJson(*doc, jsonString);
  delete doc;  // CRITICAL: Free memory

  Serial.print("pH-Chart JSON Größe: ");
  Serial.print(jsonString.length());
  Serial.println(" Bytes");

  return jsonString;
}

// WebSocket-Event-Handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (!client) {
    Serial.println("[FEHLER] WS: client ist NULL!");
    return;
  }

  switch (type) {
    case WS_EVT_DISCONNECT:
      Serial.printf("[WS] Client %u getrennt\n", client->id());
      historyDaysInitialized = false;
      break;

    case WS_EVT_CONNECT:
      {
        IPAddress ip = client->remoteIP();
        Serial.printf("[WS] Client %u verbunden von %d.%d.%d.%d\n", client->id(), ip[0], ip[1], ip[2], ip[3]);
        // Beim Connect NICHTS senden — Client fordert Daten über "init" an
      }
      break;

    case WS_EVT_DATA:
      {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len) {

        if (len > 32768) {
          Serial.printf("[WS] Message zu groß von Client %u: %zu bytes\n", client->id(), len);

          DynamicJsonDocument errorDoc(256);
          errorDoc["type"] = "error";
          errorDoc["message"] = "Nachricht zu groß";
          String errorJson;
          serializeJson(errorDoc, errorJson);
          client->text(errorJson);
          return;
        }

        Serial.printf("[WS] Nachricht von Client %u: %zu bytes\n", client->id(), len);

        // Dynamische JSON-Puffergröße basierend auf Message-Länge
        size_t bufferSize = std::max(size_t(512), std::min(size_t(32768), len * 2));
        DynamicJsonDocument doc(bufferSize);

        // data ist nicht null-terminiert — deserializeJson mit Längenbegrenzung aufrufen
        DeserializationError error = deserializeJson(doc, (const char*)data, len);

        if (error) {
          Serial.printf("[WS] JSON Parse Error von Client %u: %s\n", client->id(), error.c_str());

          DynamicJsonDocument errorDoc(256);
          errorDoc["type"] = "error";
          errorDoc["message"] = "Ungültiges JSON-Format";
          String errorJson;
          serializeJson(errorDoc, errorJson);
          client->text(errorJson);
          return;
        }

        String action = doc["action"];

        if (action == "init") {
          // Client ist bereit und fordert Initial-Daten an
          Serial.printf("[WS] Client %u fordert init an\n", client->id());

          // NUR Status senden — Settings kommen über die Queue
          String statusJson = getPumpStatusJson();
          client->text(statusJson);

          if (timeInitialized) {
            TaskItem t; t.type = TASK_SEND_SYSTEM_SETTINGS;
            t.clientId = client->id();
            enqueueTask(t);
          }

          // Schwankungskompensation-Status senden
          {
            DynamicJsonDocument factorDoc(512);
            factorDoc["type"] = "dosingFactors";
            factorDoc["enabled"] = dosingFactorsEnabled;
            factorDoc["hasNewPattern"] = hasNewPattern;
            JsonArray fArr = factorDoc.createNestedArray("factors");
            for (int i = 0; i < 12; i++) fArr.add(dosingFactors[i]);
            String factorJson;
            serializeJson(factorDoc, factorJson);
            client->text(factorJson);
          }
        } else if (action == "getStatus") {
          // Status an Client senden
          String statusJson = getPumpStatusJson();
          client->text(statusJson);
        } else if (action == "calibrate") {
          int pumpIndex = doc["pump"];
          int steps = doc["steps"];

          if (steps <= 0) steps = DEFAULT_CALIBRATION_STEPS;

          if (pumpIndex >= 0 && pumpIndex < 4) {
            bool calibrationStarted = calibratePump(pumpIndex, steps);

            if (calibrationStarted) {
              // Erfolgsmeldung an Client senden
              DynamicJsonDocument responseDoc(256);
              responseDoc["type"] = "info";
              responseDoc["message"] = "Kalibrierung für " + pumps[pumpIndex].name + " gestartet. " + String(steps) + " Schritte werden gefahren.";

              String responseJson;
              serializeJson(responseDoc, responseJson);
              client->text(responseJson);

              // Aktuellen Status senden
              String statusJson = getPumpStatusJson();
              client->text(statusJson);
            } else {
              // Fehlermeldung an Client senden
              DynamicJsonDocument errorDoc(256);
              errorDoc["type"] = "error";
              errorDoc["message"] = "Kalibrierung konnte nicht gestartet werden. Möglicherweise ist eine andere Pumpe aktiv.";

              String errorJson;
              serializeJson(errorDoc, errorJson);
              client->text(errorJson);
            }
          } else {
            // Fehlermeldung an Client senden
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Ungültige Pumpe ausgewählt.";

            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        } else if (action == "saveCalibration") {
          int pumpIndex = doc["pump"];
          float ml = doc["ml"];

          if (pumpIndex >= 0 && pumpIndex < 4 && ml > 0) {
            yield();  // Watchdog Reset vor LittleFS-Write (savePumpsToJson)
            bool calibrationSaved = saveCalibration(pumpIndex, ml);

            if (calibrationSaved) {
              // Berechne Schritte/ml für die Anzeige mit verfügbaren Variablen
              float stepsPerMl = currentCalibrationSteps / ml;                     // currentCalibrationSteps ist verfügbar
              char stepsPerMlStr[10];                                              // Buffer für formatierte Zahl
              snprintf(stepsPerMlStr, sizeof(stepsPerMlStr), "%.2f", stepsPerMl);  // Auf 2 Dezimalstellen formatieren

              // Erfolgsmeldung an Client senden
              DynamicJsonDocument responseDoc(256);
              responseDoc["type"] = "success";
              responseDoc["message"] = "Kalibrierung für " + pumps[pumpIndex].name + " gespeichert: " + String(stepsPerMlStr) + " Schritte/ml, Datum: " + formatDateTime(pumps[pumpIndex].lastCalibrationDate);

              String responseJson;
              serializeJson(responseDoc, responseJson);
              client->text(responseJson);

              // Aktuellen Status senden
              String statusJson = getPumpStatusJson();
              client->text(statusJson);
            } else {
              // Fehlermeldung an Client senden
              DynamicJsonDocument errorDoc(256);
              errorDoc["type"] = "error";
              errorDoc["message"] = "Kalibrierung konnte nicht gespeichert werden. Bitte stellen Sie sicher, dass eine gültige Kalibrierung durchgeführt wurde.";

              String errorJson;
              serializeJson(errorDoc, errorJson);
              client->text(errorJson);
            }
          } else {
            // Fehlermeldung an Client senden
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Ungültige Werte für die Kalibrierung.";

            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        } else if (action == "setGlobalSettings") {
          float speedML = doc["speedML"];
          float accelerationML = doc["accelerationML"];

          yield();  // Watchdog Reset vor LittleFS-Write
          setGlobalSettings(speedML, accelerationML);

          // Erfolgsmeldung an Client senden
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = "Globale Einstellungen gespeichert: " + String(speedML, 4) + " ml/min, " + String(accelerationML, 4) + " ml/min²";

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);

          // Aktuellen Status senden
          String statusJson = getPumpStatusJson();
          client->text(statusJson);
        } else if (action == "setTimeOffset") {
          int newOffset = doc["offset"];

          // Offset speichern (LittleFS read+write)
          yield();  // Watchdog Reset vor LittleFS-Operation
          saveTimeOffset(newOffset);

          // Erfolgsmeldung an Client senden
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = "Zeit-Offset auf " + String(newOffset) + " Sekunden gesetzt";

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);

          // Aktuelle Zeit mit neuem Offset senden
          time_t currentTime = getCurrentTime();

          DynamicJsonDocument timeDoc(256);
          timeDoc["type"] = "timeUpdate";
          timeDoc["timestamp"] = currentTime;
          timeDoc["formattedTime"] = formatDateTime(currentTime);
          timeDoc["timeInitialized"] = timeInitialized;
          timeDoc["timeOffset"] = timeOffset;

          String timeJson;
          serializeJson(timeDoc, timeJson);
          client->text(timeJson);

          // Settings-Update in Queue (mit neuem timeOffset)
          { TaskItem t; t.type = TASK_SEND_SYSTEM_SETTINGS; t.clientId = client->id(); enqueueTask(t); }
        } else if (action == "dispense") {
          int pumpIndex = doc["pump"];
          float ml = doc["ml"];
          bool isAutomatic = doc["isAutomatic"] | false;

          if (pumpIndex >= 0 && pumpIndex < 4 && ml > 0) {
            if (pumps[pumpIndex].mlPerStep > 0) {
              dispensePump(pumpIndex, ml, isAutomatic);

              // Statusmeldung statt Erfolgsmeldung
              DynamicJsonDocument responseDoc(256);
              responseDoc["type"] = "info";
              responseDoc["message"] = "Dosierung gestartet: " + String(ml) + " ml von " + pumps[pumpIndex].name;
              responseDoc["updateDosage"] = false;  // Noch nicht aktualisieren

              String responseJson;
              serializeJson(responseDoc, responseJson);
              client->text(responseJson);

              // Aktuellen Status senden
              String statusJson = getPumpStatusJson();
              client->text(statusJson);
            } else {
              // Fehlermeldung an Client senden
              DynamicJsonDocument errorDoc(256);
              errorDoc["type"] = "error";
              errorDoc["message"] = "Pumpe " + pumps[pumpIndex].name + " ist nicht kalibriert.";

              String errorJson;
              serializeJson(errorDoc, errorJson);
              client->text(errorJson);
            }
          } else {
            // Fehlermeldung an Client senden
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Ungültige Werte für die Dosierung.";

            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        } else if (action == "refillContainer") {
          int containerIndex = doc["container"];

          if (containerIndex >= 0 && containerIndex < 4) {
            refillContainer(containerIndex);

            // Erfolgsmeldung an Client senden
            DynamicJsonDocument responseDoc(256);
            responseDoc["type"] = "success";
            responseDoc["message"] = "Kanister " + pumps[containerIndex].name + " wurde aufgefüllt";

            String responseJson;
            serializeJson(responseDoc, responseJson);
            client->text(responseJson);

            // Settings-Update in Queue
            { TaskItem t; t.type = TASK_SEND_SYSTEM_SETTINGS; t.clientId = client->id(); enqueueTask(t); }
          } else {
            // Fehlermeldung an Client senden
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Ungültiger Kanister-Index";

            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        }
        // Auto-KH-Wert in reguläre KH-Messungen übernehmen
        else if (action == "adoptAutoKH") {
          int index = doc["index"];
          TaskItem t;
          t.type = TASK_ADOPT_AUTO_KH;
          t.intParam1 = index;
          t.clientId = client->id();
          enqueueTask(t);
          // Leichtgewichtige Bestätigung
          client->text("{\"type\":\"success\",\"message\":\"KH-Wert wird übernommen...\"}");
        }
        // Schwankungskompensation: Faktoren berechnen
        else if (action == "calculateDosingFactors") {
          TaskItem t;
          t.type = TASK_CALCULATE_DOSING_FACTORS;
          t.clientId = client->id();
          enqueueTask(t);
          client->text("{\"type\":\"success\",\"message\":\"Faktoren werden berechnet...\"}");
        }
        // Schwankungskompensation: Ein/Aus schalten
        else if (action == "toggleDosingFactors") {
          TaskItem t;
          t.type = TASK_TOGGLE_DOSING_FACTORS;
          t.boolParam1 = doc["enabled"] | false;
          t.clientId = client->id();
          enqueueTask(t);
        }
        // Schwankungskompensation: Faktoren zurücksetzen
        else if (action == "resetDosingFactors") {
          TaskItem t;
          t.type = TASK_RESET_DOSING_FACTORS;
          t.clientId = client->id();
          enqueueTask(t);
          client->text("{\"type\":\"success\",\"message\":\"Faktoren werden zurückgesetzt...\"}");
        }
        // NEUER CODE: Handler für updateContainerCapacity
        else if (action == "updateContainerCapacity") {
          int containerId = doc["container"];
          float capacity = doc["capacity"];

          // Validieren
          if (containerId >= 0 && containerId < 4 && capacity > 0 && capacity <= 50000) {
            // Alte Kapazität speichern für Debug-Ausgabe
            float oldCapacity = settings.containerCapacity[containerId];

            // Neue Kapazität setzen
            settings.containerCapacity[containerId] = capacity;

            // Falls nötig, Füllstand anpassen
            if (settings.containerLevel[containerId] > capacity) {
              settings.containerLevel[containerId] = capacity;
            }

            // Änderungen verzögert in LittleFS speichern (nicht im async_tcp Task!)
            deferredFlags.pendingSettingsSave = true;

            Serial.printf("[WS] Kanisterkapazität aktualisiert: %s von %.0f ml auf %.0f ml\n",
                            pumps[containerId].name.c_str(), oldCapacity, capacity);

            // Rückmeldung an Client
            DynamicJsonDocument responseDoc(256);
            responseDoc["type"] = "success";
            responseDoc["message"] = "Kapazität für " + pumps[containerId].name + " auf " + String(capacity) + " ml aktualisiert";

            String responseJson;
            serializeJson(responseDoc, responseJson);
            client->text(responseJson);

            // Settings-Update an alle Clients in Queue
            { TaskItem t; t.type = TASK_SEND_SYSTEM_SETTINGS; t.clientId = 0; enqueueTask(t); }
          } else {
            // Fehlermeldung bei ungültigen Werten
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Ungültiger Container oder Kapazitätswert";

            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        } else if (action == "saveWaterMeasurement") {
          float kh = doc["kh"] | 0.0f;
          float calcium = doc["calcium"] | 0.0f;
          Serial.printf("[WS] saveWaterMeasurement empfangen: kh=%.2f, calcium=%.2f\n", kh, calcium);

          // Messung(en) in Queue stellen (LittleFS + Dosierplan zu schwer für async_tcp)
          // Beide Werte separat enqueuen, da TaskItem nur einen floatParam hat
          if (kh > 0) {
            TaskItem t;
            t.type = TASK_SAVE_MEASUREMENT;
            t.floatParam = kh;
            t.boolParam1 = false;  // isCalcium = false → KH-Messung
            enqueueTask(t);
          }
          if (calcium > 0) {
            TaskItem t;
            t.type = TASK_SAVE_MEASUREMENT;
            t.floatParam = calcium;
            t.boolParam1 = true;  // isCalcium = true → Calcium-Messung
            enqueueTask(t);
          }

          // Sofort Bestätigung an Client senden
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = "Messwerte werden verarbeitet...";

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);
        } else if (action == "deleteWaterData") {
          int index = doc["index"];
          int dosageType = doc["dosageType"] | -1;    // -1 ist ein ungültiger Typ
          bool isCalcium = doc["isCalcium"] | false;  // Für Messungen
          bool isAutoKh = doc["isAutoKh"] | false;    // NEU: Für Auto-KH-Messungen

          // ALLES deferred! LittleFS + JSON-Generierung blockiert async_tcp zu lange
          if (dosageType >= 0) {
            TaskItem t; t.type = TASK_DELETE_DOSAGE;
            t.intParam1 = index; t.intParam2 = dosageType;
            enqueueTask(t);
          } else if (isAutoKh) {
            TaskItem t; t.type = TASK_DELETE_AUTO_KH;
            t.intParam1 = index;
            enqueueTask(t);
          } else {
            TaskItem t; t.type = TASK_DELETE_MEASUREMENT;
            t.intParam1 = index; t.boolParam1 = isCalcium;
            enqueueTask(t);
          }

          // Sofort Bestätigung senden (leichtgewichtig)
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = "Daten werden gelöscht...";

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);
        } else if (action == "getWaterMeasurements") {
          TaskItem t; t.type = TASK_SEND_WATER_MEASUREMENTS;
          t.clientId = client->id(); t.intParam1 = doc["weeks"] | 1;
          enqueueTask(t);
        } else if (action == "getAllWaterMeasurements") {
          TaskItem t; t.type = TASK_SEND_ALL_WATER_MEASUREMENTS;
          t.clientId = client->id();
          enqueueTask(t);
        } else if (action == "getSystemSettings") {
          TaskItem t; t.type = TASK_SEND_SYSTEM_SETTINGS;
          t.clientId = client->id();
          enqueueTask(t);
        } else if (action == "saveSystemSettings") {
          JsonObject settingsObj = doc["settings"];

          float aquariumVolume = settingsObj["aquariumVolume"];
          float targetKH = settingsObj["targetKH"];
          float targetCalcium = settingsObj["targetCalcium"];
          int historyCount = settingsObj["historyCount"];
          // weightingMethod wird nicht mehr verarbeitet
          float maxDailyChangeKH = settingsObj["maxDailyChangeKH"];
          float maxDailyChangeCalcium = settingsObj["maxDailyChangeCalcium"];
          float magnesiumRatio = settingsObj["magnesiumRatio"] | 50.0;
          int khNightStart = settingsObj["khNightStart"] | DEFAULT_KH_NIGHT_START;
          int khNightEnd = settingsObj["khNightEnd"] | DEFAULT_KH_NIGHT_END;
          float initialKHConsumption = settingsObj["initialKHConsumption"] | DEFAULT_INITIAL_KH_CONSUMPTION;
          float initialCalciumConsumption = settingsObj["initialCalciumConsumption"] | DEFAULT_INITIAL_CALCIUM_CONSUMPTION;
          bool autoUpdateInitialRates = settingsObj["autoUpdateInitialRates"] | true;
          bool usePhBasedKHDosing = settingsObj["usePhBasedKHDosing"] | false;
          float phThresholdForKHNight = settingsObj["phThresholdForKHNight"] | DEFAULT_PH_THRESHOLD;

          // Get container capacities
          float containerCapacity0 = settingsObj["containerCapacity0"] | DEFAULT_CONTAINER_CAPACITY;
          float containerCapacity1 = settingsObj["containerCapacity1"] | DEFAULT_CONTAINER_CAPACITY;
          float containerCapacity2 = settingsObj["containerCapacity2"] | DEFAULT_CONTAINER_CAPACITY;
          float containerCapacity3 = settingsObj["containerCapacity3"] | DEFAULT_CONTAINER_CAPACITY;

          // Anti-Tropf-Einstellungen auslesen
          bool enableAntiDrip = settingsObj["enableAntiDrip"] | DEFAULT_ENABLE_ANTI_DRIP;
          float antiDripML = settingsObj["antiDripML"] | DEFAULT_ANTI_DRIP_ML;
          float antiDripSpeedML = settingsObj["antiDripSpeedML"] | DEFAULT_ANTI_DRIP_SPEED_ML;

          // Zeit-Offset auslesen
          int newTimeOffset = settingsObj["timeOffset"] | 3600;

          saveSystemSettings(aquariumVolume, targetKH, targetCalcium, historyCount,
                             maxDailyChangeKH, maxDailyChangeCalcium,
                             magnesiumRatio, khNightStart, khNightEnd,
                             initialKHConsumption, initialCalciumConsumption,
                             autoUpdateInitialRates,
                             containerCapacity0, containerCapacity1,
                             containerCapacity2, containerCapacity3,
                             usePhBasedKHDosing, phThresholdForKHNight,
                             enableAntiDrip, antiDripML, antiDripSpeedML, newTimeOffset);

          // Erfolgsmeldung an Client senden
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = "Systemeinstellungen gespeichert";

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);

          // Settings-Update in Queue
          { TaskItem t; t.type = TASK_SEND_SYSTEM_SETTINGS; t.clientId = client->id(); enqueueTask(t); }
        } else if (action == "setAutoDosing") {
          bool autoDosing = doc["autoDosing"];

          // Nur Setting ändern, NICHT den Dosierplan im WS-Handler berechnen!
          settings.autoDosing = autoDosing;
          deferredFlags.pendingSettingsSave = true;
          { TaskItem t; t.type = TASK_UPDATE_DOSAGE_PLANS; t.boolParam2 = true; t.boolParam3 = true; enqueueTask(t); }

          Serial.printf("[WS] Auto-Dosierung %s (deferred)\n", autoDosing ? "aktiviert" : "deaktiviert");

          // Nur leichtgewichtige Bestätigung senden — Settings kommen nach Dosierplan-Update
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = String("Automatische Dosierung ") + (autoDosing ? "aktiviert" : "deaktiviert");

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);
        } else if (action == "getAllKhDosagePlan") {
          TaskItem t; t.type = TASK_SEND_KH_DOSAGE_PLAN;
          t.clientId = client->id();
          enqueueTask(t);
        } else if (action == "getAllCaDosagePlan") {
          TaskItem t; t.type = TASK_SEND_CA_DOSAGE_PLAN;
          t.clientId = client->id();
          enqueueTask(t);
        } else if (action == "getDosagePlan") {
          // Beide Pläne als separate Tasks einstellen
          { TaskItem t; t.type = TASK_SEND_KH_DOSAGE_PLAN; t.clientId = client->id(); enqueueTask(t); }
          { TaskItem t; t.type = TASK_SEND_CA_DOSAGE_PLAN; t.clientId = client->id(); enqueueTask(t); }
        } else if (action == "getDosageHistory") {
          // Prüfen, ob der Client einen Tag-Offset geschickt hat
          if (doc.containsKey("dayOffset")) {
            int newOffset = doc["dayOffset"];
            if (newOffset >= 0) {
              currentHistoryDayOffset = newOffset;
              Serial.printf("[WS] Neuer Tag-Offset empfangen: %d\n", currentHistoryDayOffset);
            }
          }
          // Deferred: getDosageHistoryJson() liest mehrere LittleFS-Dateien
          { TaskItem t; t.type = TASK_SEND_DOSAGE_HISTORY; t.clientId = client->id(); enqueueTask(t); }
        } else if (action == "saveWiFiConfig") {
          // WiFi-Konfiguration speichern
          String ssid = doc["ssid"].as<String>();
          String password = doc["password"].as<String>();

          if (ssid.length() > 0) {
            bool saved = saveWiFiConfig(ssid, password);

            DynamicJsonDocument responseDoc(256);
            if (saved) {
              responseDoc["type"] = "success";
              responseDoc["message"] = "WiFi-Konfiguration gespeichert! ESP32 startet neu...";
            } else {
              responseDoc["type"] = "error";
              responseDoc["message"] = "Fehler beim Speichern der WiFi-Konfiguration";
            }

            String responseJson;
            serializeJson(responseDoc, responseJson);
            client->text(responseJson);

            // Nach erfolgreichem Speichern: kurz warten und neu starten
            if (saved) {
              delay(1000);
              ESP.restart();
            }
          } else {
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "SSID darf nicht leer sein";
            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        } else if (action == "getWiFiConfig") {
          // Aktuelle WiFi-Konfiguration senden
          DynamicJsonDocument wifiDoc(512);
          wifiDoc["ssid"] = wifiSSID;
          wifiDoc["configured"] = wifiConfigured;
          wifiDoc["isAPMode"] = isAPMode;
          if (wifiConnected) {
            wifiDoc["ipAddress"] = WiFi.localIP().toString();
          }

          String wifiJson;
          serializeJson(wifiDoc, wifiJson);
          client->text(wifiJson);
        } else if (action == "resetSettings") {
          yield();  // Watchdog Reset vor schwerer LittleFS-Operation
          resetSettings();

          // Erfolgsmeldung an Client senden
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = "EEPROM wurde erfolgreich zurückgesetzt. Bitte Seite neu laden.";

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);

          // Status direkt, Settings in Queue
          String statusJson = getPumpStatusJson();
          client->text(statusJson);
          { TaskItem t; t.type = TASK_SEND_SYSTEM_SETTINGS; t.clientId = client->id(); enqueueTask(t); }

          // pH-Messwerte holen
        } else if (action == "getPhMeasurements") {
          TaskItem t; t.type = TASK_SEND_PH_MEASUREMENTS;
          t.clientId = client->id();
          enqueueTask(t);
        }
        // Manuell pH-Wert messen
        else if (action == "measurePh") {
          float phValue = getAveragePH();

          if (phValue >= 0 && phValue <= 14) {
            currentPH = phValue;
            yield();  // Watchdog Reset vor LittleFS-Write
            savePHMeasurement(phValue);

            // Erfolgsrückmeldung senden
            DynamicJsonDocument responseDoc(256);
            responseDoc["type"] = "success";
            responseDoc["message"] = "pH-Wert erfolgreich gemessen: " + String(phValue, 2);

            String responseJson;
            serializeJson(responseDoc, responseJson);
            client->text(responseJson);

            // Aktualisierte Messwerte in Queue
            { TaskItem t; t.type = TASK_SEND_PH_MEASUREMENTS; t.clientId = client->id(); enqueueTask(t); }
          } else {
            // Fehlermeldung senden
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Fehler bei der pH-Messung. Bitte Sonde prüfen.";

            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        }
        // pH-Sensor kalibrieren
        else if (action == "calibratePh") {
          float phValue = doc["phValue"];

          if (phValue == 4.0 || phValue == 7.0) {
            startPHCalibration(phValue);

            // Infomeldung senden
            DynamicJsonDocument responseDoc(256);
            responseDoc["type"] = "info";
            responseDoc["message"] = "Kalibrierung für pH " + String(phValue, 1) + " gestartet...";

            String responseJson;
            serializeJson(responseDoc, responseJson);
            client->text(responseJson);
          } else {
            // Fehlermeldung senden
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Ungültiger pH-Wert für Kalibrierung. Verwende pH 4,0 oder pH 7,0";

            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        }
        // pH-Messwert löschen
        else if (action == "deletePhMeasurement") {
          int index = doc["index"];

          yield();  // Watchdog Reset vor LittleFS read+write+rename
          deletePHMeasurement(index);

          // Erfolgsrückmeldung senden
          DynamicJsonDocument responseDoc(256);
          responseDoc["type"] = "success";
          responseDoc["message"] = "pH-Messwert erfolgreich gelöscht";

          String responseJson;
          serializeJson(responseDoc, responseJson);
          client->text(responseJson);

          // Aktualisierte Messwerte an alle Clients in Queue
          { TaskItem t; t.type = TASK_SEND_PH_MEASUREMENTS; t.clientId = 0; enqueueTask(t); }
        }
        // Neuer Handler für Live-Spannungsmessung
        else if (action == "getPhVoltageLive") {
          // Aktuelle Spannung senden
          String voltageJson = getPhVoltageLiveJson();
          client->text(voltageJson);
        } else if (action == "setBrowserTime") {
          // Browser-Zeit empfangen und Zeit setzen (RTC oder Time Library)
          time_t browserTime = doc["timestamp"];

          if (browserTime > 0) {
            bool success = setTimeFromBrowser(browserTime);

            DynamicJsonDocument responseDoc(256);
            if (success) {
              responseDoc["type"] = "success";
              responseDoc["message"] = rtcInitialized ?
                "Zeit in RTC synchronisiert!" :
                "Zeit in Time Library synchronisiert (geht bei Neustart verloren)!";
              responseDoc["timeInitialized"] = true;
              responseDoc["timeSource"] = rtcInitialized ? "RTC" : "TimeLib";
            } else {
              responseDoc["type"] = "error";
              responseDoc["message"] = "Fehler beim Setzen der Zeit";
              responseDoc["timeInitialized"] = false;
            }

            String responseJson;
            serializeJson(responseDoc, responseJson);
            client->text(responseJson);

            // Aktuelle Zeit an ALLE Clients senden
            if (success) {
              broadcastCurrentTime();
            }
          } else {
            DynamicJsonDocument errorDoc(256);
            errorDoc["type"] = "error";
            errorDoc["message"] = "Ungültige Browser-Zeit";
            String errorJson;
            serializeJson(errorDoc, errorJson);
            client->text(errorJson);
          }
        } else if (action == "getCurrentTime") {
          // Aktuelle Zeit an Client senden
          if (timeInitialized) {
            time_t currentTime = getCurrentTime();

            DynamicJsonDocument timeDoc(256);
            timeDoc["type"] = "timeUpdate";
            timeDoc["timestamp"] = currentTime;
            timeDoc["formattedTime"] = formatDateTime(currentTime);
            timeDoc["timeInitialized"] = true;

            String timeJson;
            serializeJson(timeDoc, timeJson);
            client->text(timeJson);
          } else {
            // Zeit noch nicht synchronisiert
            DynamicJsonDocument timeDoc(256);
            timeDoc["type"] = "timeUpdate";
            timeDoc["timeInitialized"] = false;

            String timeJson;
            serializeJson(timeDoc, timeJson);
            client->text(timeJson);
          }
        } else if (action == "getMemoryStatus") {
          Serial.println("[WS] getMemoryStatus aufgerufen");
          sendMemoryStatus();
        } else if (action == "getConsumptionData") {
          // Deferred: getAllDosages() 3x → 2+ Sekunden LittleFS-Blockade!
          { TaskItem t; t.type = TASK_SEND_CONSUMPTION_DATA; t.clientId = client->id(); enqueueTask(t); }
        } else if (action == "getPhTrendData") {
          { TaskItem t; t.type = TASK_SEND_PH_TREND; t.clientId = client->id(); enqueueTask(t); }
        } else if (action == "ping") {
          // Client-Keepalive — leichtgewichtige Antwort
          DynamicJsonDocument pongDoc(64);
          pongDoc["type"] = "pong";
          String pongJson;
          serializeJson(pongDoc, pongJson);
          client->text(pongJson);
        } else {
          // Fehlermeldung an Client senden
          DynamicJsonDocument errorDoc(256);
          errorDoc["type"] = "error";
          errorDoc["message"] = "Unbekannte Aktion: " + action;

          String errorJson;
          serializeJson(errorDoc, errorJson);
          client->text(errorJson);
        }
        }  // Ende AwsFrameInfo Check
      }
      break;

    default:
      // Nicht genutzte Nachrichtentypen ignorieren
      break;
  }
}

void handleRoot(AsyncWebServerRequest *request) {
  Serial.println("=== DEBUG: handleRoot aufgerufen ===");
  Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
  Serial.printf("Request URL: %s\n", request->url().c_str());

  // HTML-Response mit Cache-Control Header (wie im funktionierenden Backup)
  AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", WEBPAGE_HTML);
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);

  Serial.println("=== DEBUG: HTML Response gesendet ===");
}

void handleNotFound(AsyncWebServerRequest *request) {
  // HTTP-Antwort: Nicht gefunden
  request->send(404, "text/plain", "404: Not Found");
}

// Setup der WebServer-Handler
void setupWebHandlers() {
  Serial.println("=== DEBUG: Registriere HTTP Routes ===");

  server.on("/", handleRoot);
  Serial.println("  [OK] Route / -> handleRoot");

  // Vue.js offline bereitstellen
  server.on("/vue.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "application/javascript", VUEJS_SCRIPT);
    response->addHeader("Cache-Control", "public, max-age=31536000"); // 1 Jahr cachen
    request->send(response);
  });
  Serial.println("  [OK] Route /vue.js -> VUEJS_SCRIPT");

  // API-Endpunkt für Status
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String status = "{\"connected\":" + String(wifiConnected ? "true" : "false") + ",\"timeInitialized\":" + String(timeInitialized ? "true" : "false") + ",\"status\":\"" + systemStatus + "\"" + ",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    request->send(200, "application/json", status);
  });

  // API-Endpunkt für Dosierplan - gibt separate Pläne zurück
  server.on("/api/dosageplan", HTTP_GET, [](AsyncWebServerRequest *request) {
    // KRITISCHER FIX: Verwende Heap statt Stack für große JSON-Dokumente
    // Stack ist nur ~8KB, hier würden 32KB verwendet -> CRASH!

    // Hole beide Pläne als fertige JSON-Strings
    String khPlanJson = getAllKhDosagePlanJson();
    String caPlanJson = getAllCaDosagePlanJson();

    // Manuelles Zusammenbauen ohne riesige DynamicJsonDocuments
    String responseJson = "{\"khPlan\":";
    responseJson += khPlanJson;
    responseJson += ",\"caPlan\":";
    responseJson += caPlanJson;
    responseJson += "}";

    request->send(200, "application/json", responseJson);

    Serial.print("API-Anfrage: Dosierpläne gesendet (");
    Serial.print(responseJson.length());
    Serial.println(" Bytes)");
  });

  // UDP-Listener für KH-Tester starten (Port 4210)
  udpServer.begin(KH_UDP_PORT);
  Serial.printf("  [OK] UDP-Listener auf Port %d gestartet (KH-Tester)\n", KH_UDP_PORT);

  // API-Endpunkt für Event-Log
  server.on("/api/dosing_events", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Optional: maxLines Parameter (default 100)
    int maxLines = 100;
    if (request->hasParam("lines")) {
      maxLines = request->getParam("lines")->value().toInt();
      if (maxLines < 1) maxLines = 100;
      if (maxLines > 1000) maxLines = 1000;  // Limit für Performance
    }

    String logContent = getDosingEventLog(maxLines);
    request->send(200, "text/plain", logContent);

    Serial.print("API-Anfrage: Dosing Events Log gesendet (");
    Serial.print(logContent.length());
    Serial.println(" Bytes)");
  });
  Serial.println("  [OK] Route /api/dosing_events -> GET handler");

  // Standard-Handler für unbekannte URIs
  server.onNotFound(handleNotFound);

  Serial.println("=== DEBUG: Alle HTTP Routes registriert ===");
}

// OTA-Setup-Funktion (vereinfacht, da immer aktiv)
void setupOTA() {
  // Port-Defaults: 8266 für ESP8266 und 3232 für ESP32
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(ota_password);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "Sketch";
    } else {  // U_LittleFS
      type = "Dateisystem";
      // Alle Dateivorgänge beenden und LittleFS unmounten
      LittleFS.end();
    }
    Serial.println("Start OTA-Update: " + type);

    // Blockiere alle laufenden Dosierungen während des Updates
    blockCalculations = true;

    // Alle Pumpen deaktivieren
    for (int i = 0; i < 4; i++) {
      digitalWrite(ENABLE_PIN[i], HIGH);  // HIGH = deaktiviert
    }

    // Update-Status an Client senden
    DynamicJsonDocument doc(256);
    doc["type"] = "info";
    doc["message"] = "OTA-Update wird durchgeführt. Bitte nicht unterbrechen!";
    String jsonString;
    serializeJson(doc, jsonString);
    ws.textAll(jsonString);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA-Update abgeschlossen");

    // Status an Client senden
    DynamicJsonDocument doc(256);
    doc["type"] = "success";
    doc["message"] = "OTA-Update erfolgreich. Gerät startet neu...";
    String jsonString;
    serializeJson(doc, jsonString);
    ws.textAll(jsonString);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Fortschritt: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Fehler[%u]: ", error);
    String errorMsg;

    if (error == OTA_AUTH_ERROR) {
      errorMsg = "Authentifizierung fehlgeschlagen";
    } else if (error == OTA_BEGIN_ERROR) {
      errorMsg = "Initialisierung fehlgeschlagen";
    } else if (error == OTA_CONNECT_ERROR) {
      errorMsg = "Verbindungsfehler";
    } else if (error == OTA_RECEIVE_ERROR) {
      errorMsg = "Empfangsfehler";
    } else if (error == OTA_END_ERROR) {
      errorMsg = "Abschlussfehler";
    }

    Serial.println(errorMsg);

    // Fehlermeldung an Client senden
    DynamicJsonDocument doc(256);
    doc["type"] = "error";
    doc["message"] = "OTA-Update fehlgeschlagen: " + errorMsg;
    String jsonString;
    serializeJson(doc, jsonString);
    ws.textAll(jsonString);
  });

  // OTA starten
  ArduinoOTA.begin();
  Serial.println("OTA-Updates aktiviert");
}

// Helfer-Funktion, um die aktive Pumpe zu wechseln
void activatePump(int pumpIndex) {
  // Alle Pumpen deaktivieren
  for (int i = 0; i < 4; i++) {
    digitalWrite(ENABLE_PIN[i], HIGH);  // HIGH = deaktiviert
  }

  // DELAY 1: Hardware-Stabilisierung zwischen Pumpen
  delay(50);  // 50ms für ENABLE_PIN Stabilisierung

  // Nur die gewünschte Pumpe aktivieren
  if (pumpIndex >= 0 && pumpIndex < 4) {
    digitalWrite(ENABLE_PIN[pumpIndex], LOW);  // LOW = aktiviert
  }
}

// Verbrauchsdaten basierend auf vorhandenen Dosierungen
String getConsumptionDataJson() {
  // KRITISCHER FIX: Heap allocation instead of stack
  PsramJsonDocument* doc = new PsramJsonDocument(8192);
  if (!doc) {
    Serial.println("FEHLER: Heap-Allokation fehlgeschlagen!");
    return "{\"error\":\"Memory allocation failed\"}";
  }
  JsonArray consumptionArray = doc->createNestedArray("consumptionData");

  Serial.println("=== Erstelle Verbrauchsdaten ===");

  // Aktuelle Zeit holen
  time_t now = getCurrentTime();

  // Umrechnungsfaktoren
  float khPerMl = calculateKHPerML();
  float caPerMl = calculateCaPerML();

  // Hole alle Dosierungen
  int khDayCount, khNightCount, caCount;
  Dosage* khDayDosages = getAllDosages(DOSAGE_TYPE_KH_DAY, khDayCount);
  Dosage* khNightDosages = getAllDosages(DOSAGE_TYPE_KH_NIGHT, khNightCount);
  Dosage* caDosages = getAllDosages(DOSAGE_TYPE_CALCIUM, caCount);

  Serial.printf("Dosierungen: KH-Tag=%d, KH-Nacht=%d, Ca=%d\n", khDayCount, khNightCount, caCount);

  // Nur verarbeiten wenn tatsächlich Dosierungen vorhanden sind
  if (khDayCount > 0 || khNightCount > 0 || caCount > 0) {
    // Finde den ältesten und neuesten Zeitstempel
    time_t oldestTime = now;
    time_t newestTime = 0;

    for (int i = 0; i < khDayCount; i++) {
      if (khDayDosages[i].timestamp < oldestTime) oldestTime = khDayDosages[i].timestamp;
      if (khDayDosages[i].timestamp > newestTime) newestTime = khDayDosages[i].timestamp;
    }
    for (int i = 0; i < khNightCount; i++) {
      if (khNightDosages[i].timestamp < oldestTime) oldestTime = khNightDosages[i].timestamp;
      if (khNightDosages[i].timestamp > newestTime) newestTime = khNightDosages[i].timestamp;
    }
    for (int i = 0; i < caCount; i++) {
      if (caDosages[i].timestamp < oldestTime) oldestTime = caDosages[i].timestamp;
      if (caDosages[i].timestamp > newestTime) newestTime = caDosages[i].timestamp;
    }

    Serial.printf("Zeitraum: %s bis %s\n", formatDateTime(oldestTime).c_str(), formatDateTime(newestTime).c_str());

    // Erstelle Map für tägliche Dosierungen
    std::map<time_t, float> dailyKHDoses;
    std::map<time_t, float> dailyCaDoses;

    // Sammle KH-Dosierungen pro Tag
    for (int i = 0; i < khDayCount; i++) {
      time_t dayStart = getStartOfDay(khDayDosages[i].timestamp);
      dailyKHDoses[dayStart] += khDayDosages[i].amount;
    }
    for (int i = 0; i < khNightCount; i++) {
      time_t dayStart = getStartOfDay(khNightDosages[i].timestamp);
      // KH-Nacht ist doppelt konzentriert, also * 2 für echten KH-Wert
      dailyKHDoses[dayStart] += khNightDosages[i].amount * 2.0;
    }

    // Sammle Ca-Dosierungen pro Tag
    for (int i = 0; i < caCount; i++) {
      time_t dayStart = getStartOfDay(caDosages[i].timestamp);
      dailyCaDoses[dayStart] += caDosages[i].amount;
    }

    // Erstelle Datenpunkte für jeden Tag im Bereich (heute ausschließen)
    time_t startDay = getStartOfDay(oldestTime);
    time_t endDay = getStartOfDay(now);
    time_t today = getStartOfDay(now);

    for (time_t currentDay = startDay; currentDay < today; currentDay += 86400) {
      float khDoseML = 0;
      float caDoseML = 0;

      // Hole Dosierungen für diesen Tag
      if (dailyKHDoses.find(currentDay) != dailyKHDoses.end()) {
        khDoseML = dailyKHDoses[currentDay];
      }
      if (dailyCaDoses.find(currentDay) != dailyCaDoses.end()) {
        caDoseML = dailyCaDoses[currentDay];
      }

      // Nur Tage mit Dosierungen hinzufügen
      if (khDoseML > 0 || caDoseML > 0) {
        JsonObject entry = consumptionArray.createNestedObject();
        entry["timestamp"] = currentDay + 43200;      // Mittag des Tages
        entry["khConsumption"] = khDoseML * khPerMl;  // Umrechnung in dKH
        entry["caConsumption"] = caDoseML * caPerMl;  // Umrechnung in mg/l
        entry["date"] = formatDateTime(currentDay + 43200);
        entry["hasData"] = true;
        entry["isEstimate"] = false;
      }
    }
  } else {
    Serial.println("Keine Dosierungen - Graph leer");
  }

  // Speicher freigeben
  psram_delete_array(khDayDosages);
  psram_delete_array(khNightDosages);
  psram_delete_array(caDosages);

  Serial.printf("Verbrauchsdaten: %d Einträge\n", consumptionArray.size());

  String jsonString;
  serializeJson(*doc, jsonString);
  delete doc;  // CRITICAL: Free memory
  return jsonString;
}

// Heartbeat-System für robuste WebSocket-Verbindungen
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000;  // 30 Sekunden

void sendHeartbeat() {
  if (!timeInitialized) return;

  unsigned long currentMillis = millis();
  if (currentMillis - lastHeartbeat > HEARTBEAT_INTERVAL) {

    DynamicJsonDocument heartbeat(128);
    heartbeat["type"] = "ping";
    heartbeat["timestamp"] = getCurrentTime();

    String heartbeatJson;
    serializeJson(heartbeat, heartbeatJson);
    if (ws.count() > 0) {
      ws.textAll(heartbeatJson);
    }

    lastHeartbeat = currentMillis;
  }
}

// =========== SETUP UND LOOP ===========

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n=== Dosierpumpen-Steuerung startet (ESP32-S3 N16R8) ===");

  // Struct-Größen prüfen (Measurement=12, Dosage=24, PhMeasurement=12 erwartet)
  Serial.printf("sizeof(time_t)=%d, sizeof(Measurement)=%d, sizeof(Dosage)=%d, sizeof(PhMeasurement)=%d\n",
                sizeof(time_t), sizeof(Measurement), sizeof(Dosage), sizeof(PhMeasurement));

  // PSRAM-Verfügbarkeit prüfen
  if (psramFound()) {
    Serial.printf("PSRAM gefunden: %d Bytes verfuegbar\n", ESP.getFreePsram());
  } else {
    Serial.println("WARNUNG: Kein PSRAM gefunden! Grosse Allokationen nutzen internen SRAM.");
  }

  // Status-LED initialisieren
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);
  Serial.println("Debug: Status-LED konfiguriert");

  // Pin-Konfiguration für Pumpen
  Serial.println("Debug: Konfiguriere Pins...");
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);

  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);

  for (int i = 0; i < 4; i++) {
    pinMode(ENABLE_PIN[i], OUTPUT);
    digitalWrite(ENABLE_PIN[i], HIGH);  // HIGH = deaktiviert
    Serial.print("Debug: ENABLE_PIN ");
    Serial.print(i);
    Serial.println(" konfiguriert");
  }

  // EEPROM wird nicht mehr benötigt, aber für Kompatibilität mit altem Code
  // hier optional initialisieren (nur falls die Migration nicht vollständig ist)
  // Serial.println("Debug: Initialisiere EEPROM...");
  // EEPROM.begin(EEPROM_SIZE);
  // Serial.println("Debug: EEPROM initialisiert");

  // LittleFS initialisieren (für Messungen, Dosierungen und jetzt auch Einstellungen)
  Serial.println("Debug: Initialisiere LittleFS...");
  if (!LittleFS.begin(true)) {
    Serial.println("Debug: LittleFS-Initialisierung fehlgeschlagen!");
    // Setze Status für Benutzer
    systemStatus = "LittleFS-Initialisierung fehlgeschlagen! Neustart erforderlich.";
    delay(5000);    // Kurze Verzögerung, damit die Meldung im Serial Monitor erscheint
    ESP.restart();  // Neustart bei LittleFS-Fehler
  } else {
    Serial.println("Debug: LittleFS initialisiert");
  }

  // RTC initialisieren
  setupRTC();

  // pH-Sensor initialisieren
  Serial.println("Debug: Initialisiere pH-Sensor...");
  setupPHSensor();  // Lädt nun pH-Kalibrierung aus JSON
  Serial.println("Debug: pH-Sensor initialisiert");

  // Engine und FastAccelStepper für jede Pumpe initialisieren
  Serial.println("Debug: Initialisiere Stepper-Motoren...");
  engine.init();

  // Da wir einen gemeinsamen STEP_PIN und DIR_PIN für alle vier Pumpen haben,
  // müssen wir einen speziellen Ansatz wählen
  // Wir erstellen einen FastAccelStepper für den gemeinsamen STEP/DIR und
  // verwenden ENABLE_PINs zum Umschalten zwischen den Pumpen
  pumpenStepper[0] = engine.stepperConnectToPin(STEP_PIN);
  if (pumpenStepper[0]) {
    pumpenStepper[0]->setDirectionPin(DIR_PIN);
    pumpenStepper[0]->setCurrentPosition(0);
    pumpenStepper[0]->setSpeedInHz(mlPerMinToStepsPerSec(DEFAULT_SPEED_ML, 0));
    pumpenStepper[0]->setAcceleration(mlPerMin2ToStepsPerSec2(DEFAULT_ACCELERATION_ML, 0));
  }

  // Die anderen Pumpen verweisen auf das gleiche Stepper-Objekt
  for (int i = 1; i < 4; i++) {
    pumpenStepper[i] = pumpenStepper[0];
  }

  // Pumpen-Konfiguration aus JSON laden statt EEPROM
  Serial.println("Debug: Lade Pumpen-Konfiguration aus JSON...");
  loadPumpsFromJson();

  // Zeit-Offset laden
  loadTimeOffset();

  // Testen, ob die Dosierungsdateien korrekt im LittleFS vorhanden sind und lesbar sind
  testFileSystemAccess();

  // RAM-Cache für letzte Dosierungszeitstempel initialisieren (vermeidet wiederholtes LittleFS-Lesen)
  initLastDosageTimeCache();

  // Systemeinstellungen laden
  Serial.println("Debug: Lade Systemeinstellungen...");
  loadSystemSettings();
  Serial.println("Debug: Systemeinstellungen geladen");

  // Dosierpläne laden
  Serial.println("Debug: KH-Dosierplan laden...");
  loadKHDosagePlan();
  Serial.println("Debug: KH-Dosierplan geladen");

  Serial.println("Debug: Calcium-Dosierplan laden...");
  loadCaDosagePlan();
  Serial.println("Debug: Calcium-Dosierplan geladen");

  // WiFi-Konfiguration laden
  Serial.println("Debug: Lade WiFi-Konfiguration...");
  bool hasWiFiConfig = loadWiFiConfig();

  if (hasWiFiConfig && wifiSSID.length() > 0) {
    // WiFi-Config vorhanden - versuche Verbindung
    systemStatus = "Verbinde mit WLAN...";
    Serial.print("Debug: Verbinde mit WLAN: ");
    Serial.println(wifiSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    // WICHTIG: Auf WiFi-Verbindung warten (max 60 Sekunden - manchmal dauert es länger!)
    Serial.print("Debug: Warte auf WiFi-Verbindung (max 60s)");
    int wifiTimeout = 0;
    while (WiFi.status() != WL_CONNECTED && wifiTimeout < 120) {  // 120 x 500ms = 60 Sekunden
      delay(500);
      Serial.print(".");
      wifiTimeout++;
    }
    Serial.println();
  } else {
    // Keine WiFi-Config - direkt AP starten
    Serial.println("Keine WiFi-Konfiguration gefunden!");
  }

  // Prüfen ob Verbindung erfolgreich
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi verbunden");
    Serial.print("IP-Adresse: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
    systemStatus = "WLAN verbunden: " + WiFi.localIP().toString();
  } else {
    // WiFi-Verbindung fehlgeschlagen oder keine Config - starte Access Point
    Serial.println("WiFi-Verbindung fehlgeschlagen oder keine Konfiguration!");
    Serial.println("Starte Access Point für WiFi-Setup...");
    startAccessPoint();
  }

  // mDNS einrichten (nur wenn WiFi verbunden)
  if (wifiConnected && MDNS.begin(hostname)) {
    Serial.println("Debug: mDNS-Responder gestartet");
  }

  // ezTime einrichten
  Serial.println("Debug: Initialisiere ezTime...");
  setDebug(INFO);
  setInterval(3600);  // Update NTP alle 60 Sekunden
  setServer("pool.ntp.org");

  // Versuche anfängliche NTP-Synchronisierung (nur wenn WiFi verbunden)
  if (wifiConnected) {
    Serial.println("Debug: Versuche NTP-Synchronisierung...");
    if (timeStatus() != timeSet) {
      updateNTP();  // Ersten NTP-Update anstoßen
    }

    // Setze Timezone auf Europa/Berlin
    Europe_Berlin.setLocation("Europe/Berlin");
    events();  // Prozessiere ezTime events

    if (timeStatus() == timeSet) {
      Serial.println("Debug: ezTime Synchronisierung erfolgreich");
      timeInitialized = true;
      systemStatus = "Zeit synchronisiert!";

      // Debug-Info
      Serial.print("UTC-Zeit: ");
      Serial.println(UTC.dateTime(PSTR("d.m.Y H:i:s")));
      Serial.print("Lokale Zeit: ");
      Serial.println(Europe_Berlin.dateTime(PSTR("d.m.Y H:i:s")));
    } else {
      Serial.println("Debug: ezTime Synchronisierung fehlgeschlagen, versuche im Hintergrund");
      systemStatus = "Zeit nicht synchronisiert, versuche im Hintergrund...";
    }
  }

  // OTA-Updates einrichten (nur wenn WiFi verbunden)
  if (wifiConnected) {
    Serial.println("Debug: Richte OTA-Updates ein...");
    setupOTA();
    Serial.println("Debug: OTA-Updates eingerichtet");
  }

  // Setup der WebServer-Handler
  Serial.println("Debug: Richte HTTP-Server ein...");
  setupWebHandlers();

  // HTTP-Server und WebSocket starten (WiFi oder AP-Modus)
  if (wifiConnected || isAPMode) {
    // WICHTIG: WebSocket-Handler ZUERST registrieren, DANN server.begin()!
    // Sonst Race-Condition: Clients verbinden sich bevor Handler bereit sind
    Serial.println("Debug: Registriere WebSocket-Handler...");
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    Serial.println("Debug: WebSocket-Handler registriert");

    Serial.println("Debug: Starte HTTP-Server...");
    server.begin();
    Serial.println("Debug: HTTP-Server gestartet auf Port 80");

    systemStatus = "System bereit!";
    Serial.println("=== System vollständig initialisiert und bereit! ===");
  } else {
    systemStatus = "System bereit, aber kein WLAN!";
    Serial.println("=== System initialisiert, aber WLAN-Verbindung fehlgeschlagen! ===");
  }
}

void loop() {
  // OTA-Handler - hat immer höchste Priorität
  ArduinoOTA.handle();

  // KRITISCH: Stepper-Ausführung hat zweithöchste Priorität
  if (activePumpIndex >= 0) {
    // NTP-Updates blockieren während Pumpen laufen
    blockNtpUpdates = true;

    // Bei FastAccelStepper ist kein manuelles run() mehr nötig,
    // da die Schritte durch den ESP32-Timer im Hintergrund ausgeführt werden
    if (pumpenStepper[activePumpIndex]) {
      // Prüfen, ob der Motor noch läuft
      if (pumpenStepper[activePumpIndex]->isRunning() == 0) {
        updatePumpOperations();
        // Pumpe fertig, NTP wieder erlauben
        blockNtpUpdates = false;
      }
    }
  } else {
    // Keine Pumpe aktiv, NTP erlauben
    blockNtpUpdates = false;
  }

  // pH-Spannung kontinuierlich aktualisieren - unabhängig von anderen Aktivitäten
  updatePhVoltage();

  // Statische Variablen für Timing-Kontrolle
  static unsigned long lastHTTPCheck = 0;
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastTimeCheck = 0;
  static unsigned long lastPhCheck = 0;
  static unsigned long lastCleanupTime = 0;
  static unsigned long lastTimeUpdate = 0;

  unsigned long currentMillis = millis();

  // Adaptive Timing-Intervalle basierend auf Pumpenaktivität
  int httpInterval = activePumpIndex >= 0 ? 250 : 150;
  int timeCheckInterval = activePumpIndex >= 0 ? 500 : 250;

  // ezTime Events NUR wenn keine kritische Pumpenaktion läuft
  if (!blockNtpUpdates && currentMillis - lastTimeCheck > timeCheckInterval) {
    events();
    lastTimeCheck = currentMillis;

    // Zeit-Initialisierung prüfen
    if (!timeInitialized && timeStatus() == timeSet) {
      timeInitialized = true;
      systemStatus = "Zeit synchronisiert, System bereit";
      Serial.println("NTP-Zeitsynchronisierung erfolgreich!");
      Serial.print("Lokale Zeit: ");
      Serial.println(Europe_Berlin.dateTime(PSTR("d.m.Y H:i:s")));

      calculateConsumption();
      updateDosagePlans(true, true);
    }
  }

  // WebSocket-Cleanup: Tote Clients alle 2 Sekunden entfernen
  // (75ms war zu aggressiv und konnte langsame Clients als "tot" markieren)
  static unsigned long lastWsCleanup = 0;
  if (currentMillis - lastWsCleanup > 2000) {
    lastWsCleanup = currentMillis;
    ws.cleanupClients();
  }

  // HTTP-Server läuft asynchron - kein handleClient() mehr nötig!

  // =========== UDP: KH-WERTE VOM TESTER EMPFANGEN ===========
  int udpPacketSize = udpServer.parsePacket();
  if (udpPacketSize > 0) {
    char buf[256];
    int len = udpServer.read(buf, sizeof(buf) - 1);
    buf[len] = '\0';

    DynamicJsonDocument udpDoc(256);
    if (!deserializeJson(udpDoc, buf)) {
      float kh = udpDoc["kh"] | 0.0f;
      if (kh > 0) {
        // Timestamp parsen (Format: "YYYY-MM-DD HH:MM:SS")
        time_t measurementTime = 0;
        if (udpDoc.containsKey("timestamp")) {
          const char* ts = udpDoc["timestamp"];
          struct tm tm;
          memset(&tm, 0, sizeof(struct tm));
          if (sscanf(ts, "%d-%d-%d %d:%d:%d",
                     &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                     &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            measurementTime = mktime(&tm);
          }
        }

        // In Task-Queue stellen
        TaskItem t;
        t.type = TASK_SAVE_AUTO_KH;
        t.floatParam = kh;
        t.intParam1 = (int)measurementTime;
        enqueueTask(t);

        // ACK an KH-Tester senden
        udpServer.beginPacket(udpServer.remoteIP(), udpServer.remotePort());
        udpServer.print("{\"ok\":true}");
        udpServer.endPacket();

        Serial.printf("[UDP] Auto-KH empfangen: %.2f dKH (in Queue)\n", kh);
      }
    }
  }

  // =========== TASK-QUEUE PROCESSING ===========
  // Maximal EIN Task pro loop()-Durchlauf → verhindert Watchdog-Timeout.
  // Settings-Save als einfaches Flag separat (wird von vielen Stellen gesetzt).

  if (deferredFlags.pendingSettingsSave) {
    deferredFlags.pendingSettingsSave = false;
    saveSettingsToJson();
  }

  if (hasQueuedTasks()) {
    TaskItem task = dequeueTask();

    switch (task.type) {
      case TASK_SAVE_MEASUREMENT: {
        Serial.printf("[Queue] Verarbeite %s-Messwert: %.2f\n",
                         task.boolParam1 ? "Calcium" : "KH", task.floatParam);
        saveMeasurement(task.floatParam, task.boolParam1);
        yield();
        // Aktualisierte Messwerte an alle Clients senden (PSRAM + Chunked)
        if (ws.count() > 0) {
          PsramPrint buf;
          buildAllWaterMeasurementsJson(buf);
          sendPsramChunked(buf.data(), buf.size(), "water", 0);
        }
        break;
      }

      case TASK_UPDATE_DOSAGE_PLANS: {
        Serial.println("[Queue] Dosierplan-Neuberechnung...");
        calculateConsumption();
        yield();
        updateDosagePlans(task.boolParam2, task.boolParam3);
        break;
      }

      case TASK_DELETE_MEASUREMENT: {
        Serial.printf("[Queue] Lösche Messwert Index %d\n", task.intParam1);
        deleteMeasurement(task.intParam1, task.boolParam1);
        yield();
        if (ws.count() > 0) {
          PsramPrint buf;
          buildAllWaterMeasurementsJson(buf);
          sendPsramChunked(buf.data(), buf.size(), "water", 0);
          yield();
          String settingsJson = getSystemSettingsJson();
          ws.textAll(settingsJson);
        }
        break;
      }

      case TASK_DELETE_DOSAGE: {
        Serial.printf("[Queue] Lösche Dosierung Index %d, Typ %d\n", task.intParam1, task.intParam2);
        deleteDosage(task.intParam1, task.intParam2);
        yield();
        if (ws.count() > 0) {
          String historyJson = getDosageHistoryJson();
          ws.textAll(historyJson);
        }
        break;
      }

      case TASK_DELETE_AUTO_KH: {
        Serial.printf("[Queue] Lösche Auto-KH Index %d\n", task.intParam1);
        deleteAutoKHMeasurement(task.intParam1);
        yield();
        if (ws.count() > 0) {
          PsramPrint buf;
          buildAllWaterMeasurementsJson(buf);
          sendPsramChunked(buf.data(), buf.size(), "water", 0);
        }
        // Schwankungsmuster neu prüfen (Auto-KH Eintrag wurde gelöscht)
        yield();
        bool oldHasPatternDel = hasNewPattern;
        detectKHPattern();
        if (hasNewPattern != oldHasPatternDel) {
          DynamicJsonDocument factorDoc(128);
          factorDoc["type"] = "dosingFactorsStatus";
          factorDoc["hasNewPattern"] = hasNewPattern;
          String factorJson;
          serializeJson(factorDoc, factorJson);
          ws.textAll(factorJson);
        }
        break;
      }

      case TASK_SEND_CONSUMPTION_DATA: {
        AsyncWebSocketClient* c = ws.client(task.clientId);
        if (c && c->status() == WS_CONNECTED) {
          String json = getConsumptionDataJson();
          yield();
          c->text(json);
        }
        break;
      }

      case TASK_SEND_PH_TREND: {
        AsyncWebSocketClient* c = ws.client(task.clientId);
        if (c && c->status() == WS_CONNECTED) {
          String json = getPhTrendDataJson();
          yield();
          c->text(json);
        }
        break;
      }

      case TASK_SEND_DOSAGE_HISTORY: {
        AsyncWebSocketClient* c = ws.client(task.clientId);
        if (c && c->status() == WS_CONNECTED) {
          String json = getDosageHistoryJson();
          yield();
          c->text(json);
        }
        break;
      }

      case TASK_SEND_WATER_MEASUREMENTS: {
        if (ws.client(task.clientId)) {
          PsramPrint buf;
          buildWaterMeasurementsJson(task.intParam1, buf);
          yield();
          sendPsramChunked(buf.data(), buf.size(), "water", task.clientId);
        }
        break;
      }

      case TASK_SEND_ALL_WATER_MEASUREMENTS: {
        if (ws.client(task.clientId)) {
          PsramPrint buf;
          buildAllWaterMeasurementsJson(buf);
          yield();
          sendPsramChunked(buf.data(), buf.size(), "water", task.clientId);
        }
        break;
      }

      case TASK_BROADCAST_ALL_DATA: {
        if (ws.count() > 0) {
          PsramPrint buf;
          buildAllWaterMeasurementsJson(buf);
          sendPsramChunked(buf.data(), buf.size(), "water", 0);
        }
        break;
      }

      case TASK_SEND_KH_DOSAGE_PLAN: {
        AsyncWebSocketClient* c = ws.client(task.clientId);
        if (c && c->status() == WS_CONNECTED) {
          sendKHDosagePlanToClient(c);
        }
        break;
      }

      case TASK_SEND_CA_DOSAGE_PLAN: {
        AsyncWebSocketClient* c = ws.client(task.clientId);
        if (c && c->status() == WS_CONNECTED) {
          sendCaDosagePlanToClient(c);
        }
        break;
      }

      case TASK_SEND_SYSTEM_SETTINGS: {
        String json = getSystemSettingsJson();
        if (task.clientId == 0) {
          if (ws.count() > 0) {
            ws.textAll(json);
          }
        } else {
          AsyncWebSocketClient* c = ws.client(task.clientId);
          if (c && c->status() == WS_CONNECTED) {
            c->text(json);
          }
        }
        break;
      }

      case TASK_SEND_PH_MEASUREMENTS: {
        PsramPrint buf;
        buildPHMeasurementsJson(buf);
        yield();
        sendPsramChunked(buf.data(), buf.size(), "ph", task.clientId);
        break;
      }

      case TASK_SAVE_AUTO_KH: {
        // Auto-KH vom Tester: floatParam=kh, intParam1=timestamp (cast von time_t)
        time_t measurementTime = (time_t)task.intParam1;
        bool saveSuccess = saveAutoKHMeasurement(task.floatParam, measurementTime);
        if (saveSuccess) {
          Serial.printf("[Queue] Auto-KH gespeichert: %.2f dKH\n", task.floatParam);
          // Aktualisierte Messwerte an alle Clients senden (PSRAM + Chunked)
          yield();
          if (ws.count() > 0) {
            PsramPrint buf;
            buildAllWaterMeasurementsJson(buf);
            sendPsramChunked(buf.data(), buf.size(), "water", 0);
          }
          // Schwankungsmuster prüfen
          yield();
          bool oldHasPattern = hasNewPattern;
          detectKHPattern();
          if (hasNewPattern != oldHasPattern) {
            DynamicJsonDocument factorDoc(128);
            factorDoc["type"] = "dosingFactorsStatus";
            factorDoc["hasNewPattern"] = hasNewPattern;
            String factorJson;
            serializeJson(factorDoc, factorJson);
            ws.textAll(factorJson);
          }
        } else {
          Serial.println("[Queue] FEHLER: Auto-KH speichern fehlgeschlagen");
        }
        break;
      }

      case TASK_CALCULATE_DOSING_FACTORS: {
        Serial.println("[Queue] Berechne Schwankungskompensation-Faktoren");
        calculateDosingFactorsFromPattern();
        // Aktualisierte Faktoren an alle Clients senden
        DynamicJsonDocument factorDoc(512);
        factorDoc["type"] = "dosingFactors";
        factorDoc["enabled"] = dosingFactorsEnabled;
        factorDoc["hasNewPattern"] = hasNewPattern;
        JsonArray fArr = factorDoc.createNestedArray("factors");
        for (int i = 0; i < 12; i++) fArr.add(dosingFactors[i]);
        String factorJson;
        serializeJson(factorDoc, factorJson);
        ws.textAll(factorJson);
        break;
      }

      case TASK_TOGGLE_DOSING_FACTORS: {
        dosingFactorsEnabled = task.boolParam1;
        saveDosingFactors();
        Serial.printf("[Queue] Schwankungskompensation: %s\n", dosingFactorsEnabled ? "AKTIV" : "INAKTIV");
        // Status an alle Clients senden
        DynamicJsonDocument factorDoc(512);
        factorDoc["type"] = "dosingFactors";
        factorDoc["enabled"] = dosingFactorsEnabled;
        factorDoc["hasNewPattern"] = hasNewPattern;
        JsonArray fArr = factorDoc.createNestedArray("factors");
        for (int i = 0; i < 12; i++) fArr.add(dosingFactors[i]);
        String factorJson;
        serializeJson(factorDoc, factorJson);
        ws.textAll(factorJson);
        break;
      }

      case TASK_RESET_DOSING_FACTORS: {
        Serial.println("[Queue] Setze Schwankungskompensation zurück");
        // Alle Faktoren auf 1.0
        for (int i = 0; i < 12; i++) {
          dosingFactors[i] = 1.0;
          patternChangeRates[i] = 0;
        }
        dosingFactorsEnabled = false;
        lastUsedPatternEnd = 0;
        lastFactorCalculation = 0;
        hasNewPattern = false;
        saveDosingFactors();
        // Muster neu prüfen (vielleicht gibt es schon ein gültiges)
        detectKHPattern();
        // Status an alle Clients senden
        DynamicJsonDocument factorDoc(512);
        factorDoc["type"] = "dosingFactors";
        factorDoc["enabled"] = dosingFactorsEnabled;
        factorDoc["hasNewPattern"] = hasNewPattern;
        JsonArray fArr = factorDoc.createNestedArray("factors");
        for (int i = 0; i < 12; i++) fArr.add(dosingFactors[i]);
        String factorJson;
        serializeJson(factorDoc, factorJson);
        ws.textAll(factorJson);
        break;
      }

      case TASK_ADOPT_AUTO_KH: {
        Serial.printf("[Queue] Übernehme Auto-KH Index %d\n", task.intParam1);
        adoptAutoKHMeasurement(task.intParam1);
        yield();
        // Aktualisierte Daten an alle Clients broadcasten (PSRAM + Chunked)
        if (ws.count() > 0) {
          PsramPrint buf;
          buildAllWaterMeasurementsJson(buf);
          sendPsramChunked(buf.data(), buf.size(), "water", 0);
        }
        // Schwankungsmuster neu prüfen (Auto-KH Eintrag wurde gelöscht)
        yield();
        bool oldHasPattern = hasNewPattern;
        detectKHPattern();
        if (hasNewPattern != oldHasPattern) {
          DynamicJsonDocument factorDoc(128);
          factorDoc["type"] = "dosingFactorsStatus";
          factorDoc["hasNewPattern"] = hasNewPattern;
          String factorJson;
          serializeJson(factorDoc, factorJson);
          ws.textAll(factorJson);
        }
        break;
      }

      default:
        break;
    }
  }

  // pH-Kalibrierung, falls aktiv - auch adaptiv anpassen
  int phInterval = activePumpIndex >= 0 ? 100 : 50;
  if (isPhCalibrating && currentMillis - lastPhCheck > phInterval) {
    updatePHCalibration();
    lastPhCheck = currentMillis;
  }

  // NEU: Zeit-Updates regelmäßig senden (einmal pro Sekunde)
  if (!blockNtpUpdates && timeInitialized && currentMillis - lastTimeUpdate >= 10000) {
    // Aktuelle Zeit an Clients senden
    time_t currentTime = getCurrentTime();

    DynamicJsonDocument timeDoc(256);
    timeDoc["type"] = "timeUpdate";
    timeDoc["timestamp"] = currentTime;
    timeDoc["formattedTime"] = formatDateTime(currentTime);
    timeDoc["timeInitialized"] = true;

    String timeJson;
    serializeJson(timeDoc, timeJson);
    if (ws.count() > 0) {
      ws.textAll(timeJson);
    }

    lastTimeUpdate = currentMillis;
  }

  // Speicher-Status alle 5 Sekunden senden
  static unsigned long lastMemoryUpdate = 0;
  if (currentMillis - lastMemoryUpdate >= 5000) {
    sendMemoryStatus();
    lastMemoryUpdate = currentMillis;
  }

  // WLAN-Status und andere seltene Checks - auch blockieren wenn Pumpe aktiv
  if (!blockNtpUpdates && currentMillis - lastWiFiCheck > 1000) {
    // WiFi-Status überprüfen
    if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      digitalWrite(2, HIGH);  // LED ein = verbunden
      systemStatus = "WLAN verbunden, System bereit";
      Serial.println("\nWiFi verbunden");
      Serial.print("IP-Adresse: ");
      Serial.println(WiFi.localIP());
    } else if (wifiConnected && WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      digitalWrite(2, LOW);  // LED aus = nicht verbunden
      systemStatus = "WLAN-Verbindung verloren, versuche neu zu verbinden...";
      Serial.println("WiFi-Verbindung verloren. Versuche erneute Verbindung...");
    }

    // Zeit mit NTP synchronisieren (regelmäßig) - funktioniert mit UND ohne RTC
    checkTimeSync(currentMillis);

    lastWiFiCheck = currentMillis;
  }

  // Automatische Dosierung und ph-Messung - normal (minütlich) laufen lassen
  if (!blockNtpUpdates && timeInitialized && currentMillis - lastCheckTime > checkInterval) {
    lastCheckTime = currentMillis;
    checkAndPerformAutoDosing();
    schedulePhMeasurement();
    // cleanupOldData entfernt - wird jetzt nur täglich ausgeführt
  }

  // NEU: Tägliche Dateiensbereinigung
  if (!blockNtpUpdates && timeInitialized && currentMillis - lastCleanupTime > 24UL * 60UL * 60UL * 1000UL) {  // 24 Stunden (Overflow-sicher)
    Serial.println("Führe tägliche Dateiensbereinigung durch...");
    cleanupOldData();
    lastCleanupTime = currentMillis;
  }

  // Heartbeat-System für WebSocket-Stabilität
  if (!blockNtpUpdates && wifiConnected) {
    sendHeartbeat();
  }

  // Dynamischer Micro-Delay basierend auf Pumpenaktivität
  if (activePumpIndex < 0) {  // Nur wenn keine Pumpe aktiv ist
    delayMicroseconds(50);    // 50 Mikrosekunden für Background-Tasks
  }
}