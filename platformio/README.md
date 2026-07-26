# PlatformIO-Variante — CrowPanel Advance 5.0

Grund für diesen Ordner: Die Arduino-IDE-Boardverwaltung (Core "esp32" von
Espressif, Version 3.3.11) kompiliert das PSRAM fest mit **80 MHz**. Elecrows
eigenes, offizielles Beispiel für dieses Board (V1.2/V1.3) verlangt aber
**120 MHz** (`CONFIG_SPIRAM_SPEED_120M`) — das lässt sich nur über eine
andere, gepatchte PlatformIO-Plattform erreichen, nicht über ein
Arduino-IDE-Menü. Diese Diskrepanz war der wahrscheinlichste Kandidat für das
lang verfolgte Problem mit schwarzem/gestörtem Display (siehe
`firmware/README.md` und die Projektdokumentation).

## Voraussetzungen

1. **VS Code** installieren.
2. In VS Code: Extensions (die vier Quadrate in der Seitenleiste) → nach
   **"PlatformIO IDE"** suchen → installieren. Danach erscheint links ein
   neues PlatformIO-Symbol (Ameisenkopf).
3. In VS Code: **Datei → Ordner öffnen** → `platformio/stage1_display_touch_buzzer`
   auswählen (NICHT das gesamte Repo — jedes PlatformIO-Projekt ist ein
   eigener Ordner mit eigener `platformio.ini`).

## Bauen und hochladen

Über die PlatformIO-Seitenleiste (Ameisenkopf-Symbol) → "PROJECT TASKS" →
`advance-hmi` → **Build** (Häkchen-Symbol) und **Upload** (Pfeil-Symbol) in
der blauen Statusleiste unten. Serieller Monitor: Steckdosen-Symbol daneben,
oder PlatformIO: Monitor.

Bibliotheken (`lovyan03/LovyanGFX@1.2.26`) werden beim ersten Build
automatisch heruntergeladen — kein manuelles Kopieren in einen
`libraries`-Ordner nötig wie bei der Arduino IDE.

## Struktur

| Datei/Ordner | Inhalt |
|---|---|
| `stage1_display_touch_buzzer/platformio.ini` | Elecrows offizielle Konfiguration (Platform, Board, `-DCONFIG_SPIRAM_SPEED_120M=1`, LovyanGFX-Version) |
| `stage1_display_touch_buzzer/partitions.csv` | 1:1 aus Elecrows offiziellem PlatformIO-Beispiel übernommen |
| `stage1_display_touch_buzzer/src/main.cpp` | Portierung von `firmware/01_display_touch_buzzer/01_display_touch_buzzer.ino`, inhaltlich identisch |
| `stage1_display_touch_buzzer/include/*.h` | Kopien der Header aus `firmware/01_display_touch_buzzer/` |

## Ergebnis dieses Tests entscheidet über das weitere Vorgehen

- **Display läuft jetzt zuverlässig ohne Bildfehler:** PSRAM-Takt war die
  Ursache. Die restlichen vier Stages (`firmware/02_wifi_espnow` bis
  `firmware/05_pn532_spi`) werden dann ebenfalls nach PlatformIO portiert.
- **Problem besteht weiterhin:** PSRAM-Takt war nicht die (alleinige)
  Ursache — dann bleibt es bei der Arduino-IDE-Toolchain und wir suchen an
  anderer Stelle weiter (z. B. Signalintegrität/Verkabelung, weitere
  LovyanGFX-Timing-Parameter).
