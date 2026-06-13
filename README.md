# Aquarium Rechner

Werkzeuge für Meerwasser-Aquaristik — von Wasserwechsel-Berechnung bis hin zu vollautomatischer Kalk-Dosierung mit eigener ESP32-Firmware.

## Live

- **Übersicht**: https://aquarium-rechner.web.app/
- **C&R Aquarium Rechner**: https://aquarium-rechner.web.app/cr-rechner/
- **Spurenelemente Rechner**: https://aquarium-rechner.web.app/spurenrechner/
- **Kalkmanagement**: https://aquarium-rechner.web.app/kalk/

## Inhalt

### C&R Aquarium Rechner
Berechnet optimale Wasserwechsel mit C&R Lösungen, um ICP-Messwerte schrittweise zu Optimalwerten zu führen. Mit Max-Δ-Begrenzung pro Schritt für schonende Anpassung empfindlicher Korallen.

### Spurenelemente Rechner
Plant Tagesdosen einzelner Spurenelemente basierend auf ICP-Messwerten und Verbrauchshistorie. Daten lokal oder im eigenen Account.

### Kalkmanagement (Web + ESP32-Firmware)
Komplette Steuerung für eine selbstgebaute 4-Pumpen-Dosier-Anlage (Calcium, Magnesium, KH-Tag, KH-Nacht). Web-Interface plant die Dosierung, ESP32 führt sie aus.

**Web-Funktionen:**
- Quick-Inputs für KH/Ca direkt auf der Übersicht + Live-Kanister-Balken
- Dosier-Plan-Berechnung (KH + Ca/Mg) mit wählbarer Frequenz (2–12 pro Tag)
- Plan-Visualisierung als Tabelle + Charts
- Pumpen-Kalibrierung (Live-Lauf über das Web)
- pH-Sensor-Kalibrierung (2-Punkt mit pH-4 + pH-7)
- Container-Füllstand-Tracking
- Historie (Messungen, Dosierungen) mit Charts
- JSON-Backup/-Restore
- OTA-Firmware-Update mit einem Klick

**ESP32-Firmware:**
- Plan-Cache lokal (NVS) → läuft offline beliebig lange weiter (Anpassungs-Doses anhand absoluter Timestamps, danach unbegrenzte Erhaltungs-Dose)
- DS3231-RTC-Sync → Dosier-Plan läuft auch nach Stromausfall ohne Internet weiter
- Auto-Dosierung (per Schalter im Web aktivierbar)
- Anti-Drip-Sequenz (Prime → Dose → Retract) für sauberes Stop-Verhalten
- KH-Tag/Nacht-Umschaltung anhand pH-Wert oder Uhrzeit
- WS2812-Status-LED zeigt System-Zustand
- Ausfall-Benachrichtigung via [healthchecks.io](https://healthchecks.io) (Email/Telegram/Discord/…)
- OTA-Update direkt aus GitHub Releases

## Hosting & Daten

Statische HTML-Seiten, ausgeliefert direkt von PocketBase (`pb_public/`). Alles läuft im Browser.

**Ohne Login** (C&R, Spuren): Daten werden ausschließlich lokal im Browser gespeichert (localStorage). Nichts wird übertragen.

**Mit Login** (E-Mail/Passwort): Daten werden zusätzlich auf einem privaten PocketBase-Server (Deutschland, Übertragung per HTTPS verschlüsselt) abgelegt, damit du sie auf jedem Gerät verfügbar hast. Jeder Nutzer kann nur seine eigenen Daten lesen/schreiben (PocketBase-API-Regeln pro Nutzer). Kein Tracking, keine Weitergabe.

**Kalkmanagement** erfordert einen Account, weil Web und ESP über PocketBase synchronisieren.

## ESP-Firmware bauen

Im Ordner [`esp-firmware/aquarium-rechner-esp/`](esp-firmware/aquarium-rechner-esp/).

- Board: **ESP32-S3 N16R8** (16 MB Flash, 8 MB OPI PSRAM)
- IDE: Arduino 2.x mit „ESP32 by Espressif Systems" Boards
- Partition: `16M Flash (3MB APP/9.9MB FATFS)`
- Libraries: FastAccelStepper, ArduinoJson (v7), RTClib, Adafruit NeoPixel (keine Firebase-Library mehr nötig — Backend per HTTPS-REST)

Pin-Belegung siehe [`config.h`](esp-firmware/aquarium-rechner-esp/config.h).

Neue Releases werden als GitHub-Releases mit kompilierter `.bin` als Asset veröffentlicht — der ESP holt sie sich per OTA automatisch.

## Lizenz

[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

Du darfst das Material teilen und bearbeiten, sofern du:
- **Namensnennung** vornimmst (Markus / markus629)
- es **nicht für kommerzielle Zwecke** nutzt
- abgeleitete Werke unter der **gleichen Lizenz** weitergibst

Vollständiger Lizenztext: [LICENSE](LICENSE)
