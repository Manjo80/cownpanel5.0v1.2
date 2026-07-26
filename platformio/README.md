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
| `stage1_display_touch_buzzer/platformio.ini` | Elecrows offizielle Konfiguration (Platform, Board, LovyanGFX-Version) |
| `stage1_display_touch_buzzer/boards/ESP32-S3-WROOM-1-N16R8.json` | **Notwendig** — eigene Board-Definition, ohne die PlatformIO die Board-ID nicht kennt ("UnknownBoard") |
| `stage1_display_touch_buzzer/sdkconfig.defaults.esp32s3` | **Die eigentlich wichtige Datei** — setzt `CONFIG_SPIRAM_SPEED_120M` als echte ESP-IDF-Kconfig-Option (nicht nur als Compiler-Define), dazu Cache-Line-Groesse und Anti-Tearing (`CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_ENABLE`) |
| `stage1_display_touch_buzzer/sdkconfig.defaults` | Fast leer (Elecrows Original enthaelt hier nur LVGL-Einstellungen, die Stage 1 nicht braucht); muss aber als Datei existieren |
| `stage1_display_touch_buzzer/partitions.csv` | 1:1 aus Elecrows offiziellem PlatformIO-Beispiel übernommen |
| `stage1_display_touch_buzzer/src/main.cpp` | Portierung von `firmware/01_display_touch_buzzer/01_display_touch_buzzer.ino`, inhaltlich identisch |
| `stage1_display_touch_buzzer/include/*.h` | Kopien der Header aus `firmware/01_display_touch_buzzer/` |

**Wichtig zum Verstaendnis:** Der `-DCONFIG_SPIRAM_SPEED_120M=1` in `build_flags`
(platformio.ini) allein haette NICHT gereicht — das ist nur ein
Compiler-Define, das den Diagnose-Print im Code beeinflusst, aber nicht die
tatsaechliche Hardware-Taktung des PSRAM-Treibers aendert. Die echte
Aenderung passiert ueber `sdkconfig.defaults.esp32s3`, das PlatformIO beim
Bauen automatisch erkennt und in die zugrundeliegende ESP-IDF-Konfiguration
einspeist (Arduino laeuft unter diesem Core als ESP-IDF-Komponente).

## Ergebnis dieses Tests entscheidet über das weitere Vorgehen

- **Display läuft jetzt zuverlässig ohne Bildfehler:** PSRAM-Takt war die
  Ursache. Die restlichen vier Stages (`firmware/02_wifi_espnow` bis
  `firmware/05_pn532_spi`) werden dann ebenfalls nach PlatformIO portiert.
- **Problem besteht weiterhin:** PSRAM-Takt war nicht die (alleinige)
  Ursache — dann bleibt es bei der Arduino-IDE-Toolchain und wir suchen an
  anderer Stelle weiter (z. B. Signalintegrität/Verkabelung, weitere
  LovyanGFX-Timing-Parameter).
