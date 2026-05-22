# Aquarium-Rechner ESP-Firmware

Schlanke Firmware für ESP32-S3 N16R8 (16 MB Flash, 8 MB PSRAM), die mit
dem [Aquarium-Rechner-Web-UI](https://aquarium-rechner.web.app) über
Firebase synchronisiert.

## Hardware

- **Board**: ESP32-S3 N16R8
- **Schritt-Motoren** (4 × Stepper für die Pumpen):
  - STEP gemeinsam: IO4
  - DIR gemeinsam:  IO5
  - ENABLE Pumpe 0 (Calcium):     IO14
  - ENABLE Pumpe 1 (Magnesium):   IO6
  - ENABLE Pumpe 2 (KH-Tag):      IO13
  - ENABLE Pumpe 3 (KH-Nacht):    IO7
- **pH-Sensor** (analoger Eingang): IO1 (ADC1_CH0)
- **DS3231 RTC** (I²C): SDA=IO8, SCL=IO9

## Erstinstallation (einmalig per USB)

1. **Arduino IDE 2.x** installieren
2. **ESP32-Board** via Boards-Manager → `esp32 by Espressif Systems`
3. **Board auswählen**: `ESP32S3 Dev Module`
4. **Partition Scheme**: `16M Flash (3MB APP/9.9MB FATFS)` — Default für N16R8
5. **PSRAM**: `OPI PSRAM` (nicht QSPI — N16R8 hat Octal PSRAM)
6. **Libraries installieren** (Library Manager):
   - `Firebase ESP Client` (von Mobizt) — Version 4.4+
   - `FastAccelStepper` (von gin66)
   - `RTClib` (von Adafruit)
   - `ArduinoJson` (von Benoit Blanchon) — Version 7.x
7. Sketch `aquarium-rechner-esp.ino` öffnen, kompilieren, hochladen

## Erster Boot — WiFi-Setup

Nach dem ersten Flash:

1. ESP startet einen Access-Point: **`AquariumRechner-Setup`**
2. Verbinde dich mit Handy/Laptop (kein Passwort)
3. Browser öffnet automatisch das Setup-Portal (Captive-Portal), oder gehe zu `http://192.168.4.1/`
4. **WLAN-Daten** + **Firebase-Login** (deine E-Mail + Passwort vom Aquarium-Rechner-Konto) eingeben
5. Speichern → ESP startet neu, verbindet sich mit deinem WLAN, meldet sich an Firebase an

Ab da an läuft alles automatisch. Im Web-UI siehst du den ESP als „online" mit Live-Heartbeat.

## Was die Firmware tut

- **Heartbeat alle 30 s**: meldet pH, Uhrzeit, freier RAM, WLAN-Signal an `users/{uid}/aquarium/info`
- **Command-Polling**: alle 30 s (oder 2 s wenn ein Command offen ist) — führt Dosierungen, Kalibrierungen, manuelle Aktionen aus
- **Plan-Ausführung**: prüft alle 60 s ob ein Dosier-Job aus dem geplanten Plan jetzt fällig ist
- **Tag/Nacht-Entscheidung**: lokal anhand pH-Wert oder Uhrzeit (je nach Einstellung)
- **Offline-Fallback**: Plan-Cache in NVS — bei WLAN-Ausfall läuft der Plan bis zu 25 h ohne Server weiter
- **Auto-OTA**: alle 6 h Check auf neue Firmware in Firebase Storage

## Setup-Modus erneut aktivieren

Wenn du WLAN-Daten oder Firebase-Login ändern willst:

- ESP **3× kurz hintereinander stromlos machen + wieder einschalten** → springt zurück in den Setup-Modus
- Oder über das Web-UI: „Gerät zurücksetzen" (in Phase 2.5 verfügbar)

## Status-LED

- **Blau blinkend langsam**: Setup-Modus (warte auf Konfiguration)
- **Blau dauerhaft**: WLAN verbunden, kein Firebase
- **Grün dauerhaft**: alles okay, ESP online
- **Gelb blinkend**: gerade am Dosieren
- **Rot blinkend**: Fehler — siehe Serial-Monitor

(LED-Pin in `config.h` einstellen)
