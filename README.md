# Aquarium Rechner

Zwei Hilfswerkzeuge für Meerwasser-Aquaristik, gehostet als statische Webseite.

## Live

- **Übersicht**: https://aquarium-rechner.web.app/
- **C&R Aquarium Rechner**: https://aquarium-rechner.web.app/cr-rechner/
- **Spurenelemente Rechner**: https://aquarium-rechner.web.app/spurenrechner/

## Inhalt

### C&R Aquarium Rechner
Berechnet optimale Wasserwechsel mit C&R Lösungen, um ICP-Messwerte schrittweise zu Optimalwerten zu führen. Mit Max-Δ-Begrenzung pro Schritt für schonende Anpassung empfindlicher Korallen.

### Spurenelemente Rechner
Plant Tagesdosen einzelner Spurenelemente basierend auf ICP-Messwerten und Verbrauchshistorie. Alle Daten werden ausschließlich lokal im Browser (localStorage) gespeichert.

## Hosting & Daten

Statische HTML-Seiten via Firebase Hosting. Alles läuft im Browser.

**Ohne Login**: Daten werden ausschließlich lokal im Browser gespeichert (localStorage). Nichts wird übertragen.

**Mit Login** (optional, E-Mail/Passwort oder Google): Daten werden zusätzlich verschlüsselt bei Google Firebase / Firestore (Region europe-west) abgelegt, damit du sie auf jedem Gerät verfügbar hast. Jeder Nutzer kann nur seine eigenen Daten lesen/schreiben (Firestore Security Rules). Kein Tracking, keine Weitergabe.

## Lizenz

[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

Du darfst das Material teilen und bearbeiten, sofern du:
- **Namensnennung** vornimmst (Markus / markus629)
- es **nicht für kommerzielle Zwecke** nutzt
- abgeleitete Werke unter der **gleichen Lizenz** weitergibst

Vollständiger Lizenztext: [LICENSE](LICENSE)
