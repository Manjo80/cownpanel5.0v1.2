# PlatformIO-Variante — CrowPanel Advance 5.0

Primäre Entwicklungsumgebung für dieses Projekt (statt Arduino IDE), aus zwei Gründen:

1. Die Arduino-IDE-Boardverwaltung kompiliert PSRAM fest mit 80 MHz, nicht
   den vom Board-Hersteller vorgesehenen 120 MHz — nur über die hier
   verlinkte, gepatchte PlatformIO-Plattform + `sdkconfig.defaults.esp32s3`
   erreichbar (siehe unten). Hat sich am Ende NICHT als Ursache der
   Bildstreifen herausgestellt, ist aber trotzdem die korrekte Taktung fürs
   Board und bleibt deshalb aktiv.
2. PlatformIO verwaltet Bibliotheken automatisch pro Projekt (kein manuelles
   Kopieren in einen globalen `libraries`-Ordner wie bei der Arduino IDE),
   was besonders bei den Patches/Versionsvorgaben aus früheren Gesprächen
   half.

Die eigentliche Ursache für die hartnäckigen Bildstreifen war ein Bounce-
Buffer-Problem (siehe `firmware/README.md`) — jeder Stage-Ordner nutzt
deshalb Espressifs `esp_lcd_panel_rgb`-Treiber statt LovyanGFX's `Bus_RGB`.

## Voraussetzungen

1. **VS Code** installieren.
2. In VS Code: Extensions (die vier Quadrate in der Seitenleiste) → nach
   **"PlatformIO IDE"** suchen → installieren. Danach erscheint links ein
   neues PlatformIO-Symbol (Ameisenkopf).
3. In VS Code: **Datei → Ordner öffnen** → den jeweiligen Stage-Ordner
   auswählen (z. B. `platformio/stage2_wifi_espnow`) — NICHT das gesamte
   Repo. Jedes PlatformIO-Projekt ist ein eigener Ordner mit eigener
   `platformio.ini`.

## Bauen und hochladen

Über die PlatformIO-Seitenleiste (Ameisenkopf-Symbol) → "PROJECT TASKS" →
`advance-hmi` → **Build** (Häkchen-Symbol) und **Upload** (Pfeil-Symbol) in
der blauen Statusleiste unten. **Wichtig: Build allein schreibt nichts aufs
Board — immer Upload (Pfeil) klicken, nicht nur das Häkchen.** Serieller
Monitor: Steckdosen-Symbol daneben, oder PlatformIO: Monitor.

Falls PlatformIO Core selbst beschädigt ist (`ModuleNotFoundError: No
module named 'click'` o. ä.): VS Code schließen, Ordner
`C:\Users\<Name>\.platformio\penv` löschen, VS Code neu öffnen — die
Extension baut die Python-Umgebung automatisch neu auf.

## Stages

| Ordner | Entspricht | Nutzt esp_lcd_panel_rgb? |
|---|---|---|
| `stage1_display_touch_buzzer/` | `firmware/01_display_touch_buzzer/` | Ja — an echter Hardware verifiziert |
| `stage2_wifi_espnow/` | `firmware/02_wifi_espnow/` | Ja — an echter Hardware verifiziert (2 Boards) |
| `stage3_rtc/` | `firmware/03_rtc/` | Ja — Erweiterung von Stage 2 (WLAN/ESP-NOW bleibt), an echter Hardware verifiziert |
| `stage4_sd_card/` | `firmware/04_sd_card/` | Ja — Erweiterung von Stage 3 (RTC, WLAN/ESP-NOW bleiben), an echter Hardware verifiziert |
| `stage5_pn532_spi/` | `firmware/05_pn532_spi/` | Ja — Erweiterung von Stage 4 (SD, RTC, WLAN/ESP-NOW bleiben), kompakteres Layout, inkl. DESFire-Tiefenauslesung (`include/desfire.h`, siehe `firmware/README.md` Abschnitt 8) — DESFire-Teil ungetestet, wird getestet |

Jeder Stage-Ordner enthält:

| Datei/Ordner | Inhalt |
|---|---|
| `platformio.ini` | Elecrows offizielle Plattform-/Board-Konfiguration + LovyanGFX-Version |
| `boards/ESP32-S3-WROOM-1-N16R8.json` | **Notwendig** — eigene Board-Definition, ohne die PlatformIO die Board-ID nicht kennt ("UnknownBoard") |
| `sdkconfig.defaults.esp32s3` | Setzt `CONFIG_SPIRAM_SPEED_120M` als echte ESP-IDF-Kconfig-Option (nicht nur Compiler-Define) |
| `sdkconfig.defaults` | Fast leer (Elecrows Original enthält hier nur LVGL-Einstellungen); muss aber als Datei existieren |
| `partitions.csv` | 1:1 aus Elecrows offiziellem PlatformIO-Beispiel übernommen |
| `src/main.cpp` | Inhaltlich identisch zum jeweiligen `firmware/0X_*/*.ino` |
| `include/rgb_panel.h` | RGB-Panel-Bringup über `esp_lcd_panel_rgb` (Bounce Buffer statt LovyanGFX Bus_RGB) |
| `include/touch_standalone.h` | GT911-Touch eigenständig, ohne LGFX-Device |
| `include/*.h` (restliche) | Pin-/Adressreferenz, Touch-Timing-Fixes, Backlight/Buzzer |

**Wichtig zum Verstehen von `CONFIG_SPIRAM_SPEED_120M`:** Der
`-DCONFIG_SPIRAM_SPEED_120M=1` in `build_flags` (platformio.ini) allein
hätte NICHT gereicht — das ist nur ein Compiler-Define für den
Diagnose-Print im Code. Die echte Hardware-Taktung passiert über
`sdkconfig.defaults.esp32s3`, das PlatformIO automatisch erkennt und in die
zugrundeliegende ESP-IDF-Konfiguration einspeist.

**Besonderheiten von `stage5_pn532_spi/`:**
- `build_flags` enthält zusätzlich `-DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1` — Entsprechung zu Arduino IDEs "USB CDC On
  Boot: Enabled", nötig weil der PN532 auf UART0-Pins (IO43/44) hängt und
  die serielle Konsole deshalb über den nativen USB-CDC-Port laufen muss.
- `lib_deps` bringt `Adafruit PN532` automatisch mit, aber NICHT die zwei
  nötigen Patches (siehe `firmware/README.md` Abschnitt 3) — die müssen
  nach dem ersten Build manuell in
  `.pio/libdeps/advance-hmi/Adafruit PN532/` nachgetragen werden.
- `include/desfire.h` nutzt `mbedtls/aes.h` (AES-Authentifizierung) und
  `esp_random.h` — beides Teil des ESP-IDF-Kerns, keine zusätzliche
  `lib_deps`-Zeile nötig. DES/2K3DES ist dagegen komplett selbst
  implementiert (kein `mbedtls/des.h`), weil `framework=arduino` mbedtls
  als vorkompiliertes Binary ausliefert, in dem `mbedtls_des3_*` fehlt —
  Details siehe `firmware/README.md` Abschnitt 8.
