# Firmware — Stufenweiser Funktionstest CrowPanel Advance 5.0

Fünf aufeinander aufbauende, einzeln flashbare Arduino-Sketches, die die
Hardware dieses Boards Schritt für Schritt aufbauen und testen — in der
Reihenfolge, in der sie üblicherweise ans Laufen gebracht werden:

| Ordner | Testet | Status |
|---|---|---|
| `01_display_touch_buzzer/` | RGB-Display, GT911-Touch, Backlight/Buzzer (STC8H1K28) | fertig, RGB-Ausgabe auf esp_lcd_panel_rgb umgebaut (siehe unten), an echter Hardware verifiziert (sauberes Bild, Touch nach Stromlos-Start OK) |
| `02_wifi_espnow/` | WLAN-Modus + ESP-NOW Senden/Empfangen | fertig, an echter Hardware verifiziert (2 Boards, inkl. Fix für Task-Race beim ESP-NOW-Empfang, siehe Abschnitt 7) |
| `03_rtc/` | PCF8563-Echtzeituhr, optional NTP-Sync — Erweiterung von Stage 2 (WLAN/ESP-NOW bleibt erhalten) | fertig, an echter Hardware verifiziert (als Teil von Stage 4 mitbestätigt) |
| `04_sd_card/` | SD-/TF-Karte: Live-Erkennung (Einstecken/Herausziehen) + fortlaufendes ESP-NOW-Log — Erweiterung von Stage 3 (RTC, WLAN/ESP-NOW bleiben erhalten) | fertig, an echter Hardware verifiziert (SD-Erkennung + Logging funktionieren) |
| `05_pn532_spi/` | PN532-NFC-Modul über Software-SPI (Firmware-Version, UID-Read, DESFire-Tiefenauslesung mit Default-Schlüssel — siehe Abschnitt 8) — Erweiterung von Stage 4 (SD, RTC, WLAN/ESP-NOW bleiben erhalten), Bildschirm auf Nutzerwunsch kompakter statt Funktionen wegzulassen | auf esp_lcd_panel_rgb umgebaut, DESFire-Teil ungetestet (kein Testgerät), wird getestet |

### Wichtige Architekturänderung (esp_lcd_panel_rgb statt LovyanGFX Bus_RGB)

Hartnäckige Bildstreifen/-fehler auf Buttons ließen sich mit reinen
LovyanGFX-Anpassungen (Sprite-Puffer, `waitDMA()`, PSRAM-Takt, Pixeltakt)
nicht beheben. Wahrscheinliche Ursache: LovyanGFX's `Bus_RGB`-Treiber hat
keinen "Bounce Buffer" — bei ESP32-S3-RGB-Panels mit PSRAM-Framebuffer ist
das ein bekanntes Problem, da CPU-Schreibzugriffe (Zeichnen) mit der
kontinuierlichen GDMA-Bildausgabe um dieselbe PSRAM-Bandbreite konkurrieren.
Espressifs eigener ESP-IDF-Treiber (`esp_lcd_panel_rgb`) hat dafür einen
Bounce Buffer (kleiner SRAM-Zwischenpuffer) eingebaut.

Alle fünf Stages nutzen deshalb jetzt `esp_lcd_panel_rgb` direkt für die
Hardware-Ausgabe (`rgb_panel.h`) und GT911-Touch eigenständig ohne
LGFX-Device (`touch_standalone.h`). LovyanGFX wird nur noch für die
Zeichen-API genutzt: `LGFX_Sprite canvas` zeigt per `setBuffer()` direkt
auf den von `esp_lcd_panel_rgb` bereitgestellten Framebuffer. Zusätzlich
wartet `setup()` 600 ms zwischen `Wire.begin()` und der ersten
Touch-Kommunikation, da der GT911 nach einem echten Stromlos-Start selbst
noch Boot-Zeit braucht (nach Reset-Knopf fällt das dort nicht auf, da der
Chip dort schon läuft).

Jeder Ordner ist ein vollständiger, eigenständiger Arduino-Sketch (Ordnername
= `.ino`-Dateiname, wie von der Arduino-IDE verlangt) und enthält seine
eigene Kopie von `pins.h` und den gemeinsam genutzten Hilfs-Headern. Die
kanonische Quelle für alle Pin-/Adresszuordnungen ist
[`common/pins.h`](common/pins.h) — bei Änderungen dort zuerst anpassen und
dann in jeden `0X_*`-Ordner kopieren.

**Empfohlenes Vorgehen:** jede Stufe einzeln flashen und auf echter Hardware
verifizieren, bevor die nächste angegangen wird. Ab Stage 2 baut jede Stufe
auf der vorherigen auf (Buttons/Status bleiben sichtbar, neue Funktionen
kommen dazu) — so ist auf einen Blick erkennbar, dass Vorheriges nicht durch
Neues kaputtgegangen ist. Stage 1 bleibt der einzige komplett eigenständige
Sketch.

---

## 1. Board-Identifikation

Elecrow CrowPanel Advance 5.0, SKU DIS02050A, ESP32-S3-WROOM-1 N16R8.
Anhand der Backlight/Buzzer-Werte (0/244/245 bzw. 246/247, siehe unten)
entspricht das der Hardware-Revision **V1.2/V1.3** aus dem offiziellen
Elecrow-Repo. Offizielles Repo (nur als Referenz, nicht als Abhängigkeit
eingebunden):
https://github.com/Elecrow-RD/CrowPanel-Advance-5-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480

## 2. Arduino-IDE-Einstellungen (Werkzeuge-Menü)

| Einstellung | Wert |
|---|---|
| Board | ESP32S3 Dev Module |
| PSRAM | **OPI PSRAM** (kritisch — ohne bleibt das Display schwarz) |
| Flash Size | 16 MB (128 Mb) |
| Partition Scheme | Huge APP (3 MB No OTA / 1 MB SPIFFS) |
| Flash Mode | QIO 80 MHz |
| CPU Frequency | 240 MHz (WiFi) |
| USB CDC On Boot | Disabled, außer bei Stage 5 (PN532 belegt UART0-Pins 43/44) → dort **Enabled** |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |

PSRAM und Partition Scheme springen bei Core-Wechseln häufig auf den
Default zurück — nach jedem Core-Wechsel prüfen.

## 3. Benötigte Libraries

| Library | Version | Für Stage |
|---|---|---|
| ESP32 Arduino Core | 3.3.11 (getestete Basis) | alle |
| LovyanGFX | **1.2.26**, nur diese Version im `libraries`-Ordner | 1, 2, 3, 4, 5 |
| Adafruit_PN532 | 1.3.4, **modifiziert** (siehe unten) | 5 |

**Wichtig:** Auf Core 3.3.11 kompiliert die ältere Elecrow-LovyanGFX (1.1.16)
nicht — Core-interne Symbole wurden umbenannt. Nur LovyanGFX 1.2.26
verwenden, und nur eine Version im `libraries`-Ordner.

### Pflicht-Patch für Adafruit_PN532 (Stage 5)

Ohne diesen Patch schlagen Antworten mit mehr als 64 Byte fehl:

1. `Adafruit_PN532.h`: `#define PN532_PACKBUFFSIZ 64` → `255`
2. `Adafruit_PN532.cpp`, Funktion `inDataExchange()`: Timeout `1000` → `5000` (ms)

## 4. Gemeinsamer I2C-Bus (Touch, RTC, Backlight/Buzzer)

`Wire.begin(15, 16, 400000)` — SDA=IO15, SCL=IO16, Header-Aufdruck
`IO16_SCL / IO15_SDA / 3V3 / GND`.

| Adresse | Gerät |
|---|---|
| 0x5D **oder** 0x14 | GT911 Touch-Controller (adressiert sich beim Power-On zufällig, siehe unten) |
| 0x51 | PCF8563 Echtzeituhr |
| 0x30 | STC8H1K28 — Backlight-, Buzzer- und Speaker-Steuerung |
| 0x24 | Elechouse PN532, nur im I2C-Modus (hier nicht verwendet, siehe Stage 5) |

### GT911-Adress-Fix (bereits in `touch_probe.h` umgesetzt)

Der GT911 wählt seine I2C-Adresse (0x5D oder 0x14) beim eigenen Power-On
anhand des Pegels an seinem INT-Pin (hier GPIO1). Elecrows Werks-Testcode
(`factory_sourcecode`) steuert diesen Pin AKTIV an: GPIO1 wird als eine der
allerersten Aktionen in `setup()` für 120 ms auf LOW gezogen und danach
wieder auf Eingang gestellt ("GT911 上电时序 ---> 选用 0x5D" — GT911-Power-On-
Sequenz, wählt 0x5D). Das erzwingt die Adresse deterministisch, statt sie
dem Zufall zu überlassen. Jeder Stage-Sketch ruft dafür `gt911PowerOnSequence()`
(in `touch_probe.h`) als erste Zeile in `setup()` auf — noch vor `Serial.begin()`,
da der GT911 seine Power-On-Reset-Sequenz parallel zum ESP32-Boot durchläuft.
`probeGT911Address()` sondiert zusätzlich zur Laufzeit beide Adressen als
Sicherheitsnetz, falls die Sequenz aus irgendeinem Grund doch nicht greift.

## 5. Backlight & Buzzer (STC8H1K28, Adresse 0x30)

Einzelnes Byte senden, **erst nach** `lcd.init()` (sonst überschreibt die
Panel-Initialisierung den Zustand):

| Wert | Funktion |
|---|---|
| 0 | Backlight maximale Helligkeit |
| 244 | Backlight minimale Helligkeit |
| 245 | Backlight aus |
| 246 | Buzzer an |
| 247 | Buzzer aus |

Siehe `backlight_buzzer.h` für fertige Hilfsfunktionen
(`setBacklightPercent()`, `buzzerBeep()`).

## 6. Wiring pro Stage

**Stage 1–3:** nur der gemeinsame I2C-Bus + das fest verdrahtete RGB-Panel,
kein zusätzliches Wiring nötig.

**Stage 4 (SD-Karte):** Hardware-DIP **Function-Select S1/S0 = 1/1** ("MIC &
TF Card") setzen. In diesem Modus: SD MISO=IO4, SCK=IO5, MOSI=IO6, CS=IO0.

**Stage 5 (PN532 SPI):** vier frei gesammelte Pins aus zwei Headern
(Software-SPI, kein Löten nötig):

```
PN532 VCC   -> 3V3   (Wireless-Header)
PN532 GND   -> GND   (Wireless-Header)
PN532 SCK   -> IO43  (UART0-OUT)
PN532 MOSI  -> IO44  (UART0-OUT)
PN532 MISO  -> IO2   (Wireless-Header)
PN532 SS    -> IO8   (Wireless-Header)
```

PN532-DIP-Schalter am Modul auf **SPI** (Sw1=OFF, Sw2=ON). Da IO43/44 =
UART0 TX/RX sind, muss "USB CDC On Boot" auf **Enabled** stehen, sonst
kollidiert die serielle Konsole mit dem PN532 (Arduino IDE: Werkzeuge-Menü;
PlatformIO: `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` in
`build_flags`, bereits in `platformio/stage5_pn532_spi/platformio.ini`
gesetzt).

SD (Stage 4, IO4/5/6) und PN532 (Stage 5, IO43/44/2/8) nutzen unterschiedliche
Pins und können bei S1/S0=1/1 gleichzeitig betrieben werden — in einem
künftigen kombinierten Sketch ist das also kein Konflikt.

## 7. Bekannte Stolpersteine

- **Display bleibt schwarz:** PSRAM nicht auf OPI PSRAM gestellt.
- **Touch reagiert nicht / Display manchmal schwarz, manchmal nicht:**
  GT911-Adresswechsel, siehe Abschnitt 4 — durch `probeGT911Address()`
  bereits behoben; falls trotzdem instabil, echten I2C-Pullup/Verkabelung
  prüfen.
- **Touch funktioniert nach Reset-Knopf, aber nicht nach echtem
  Stromlos-Neustart:** Der GT911-Chip selbst braucht nach einer echten
  Kaltstart-Stromversorgung noch Zeit für sein eigenes Power-On-Booting,
  bevor er auf dem I2C-Bus antwortet — beim Reset-Knopf läuft der Chip
  bereits, daher fällt das dort nicht auf. Stage 1 wartet deshalb
  zusätzlich zur 120ms-Adress-Auswahl-Sequenz (`gt911PowerOnSequence()`)
  weitere 600 ms, bevor der Touch-Chip angesprochen wird. Falls das auf
  einzelnen Boards nicht reicht, Wert in `setup()` erhöhen.
- **ESP-NOW-Callback kompiliert nicht:** Callback-Signaturen sind
  Core-Versions-abhängig. Dieser Code ist auf Core 3.x geschrieben
  (`esp_now_recv_info_t*`); auf Core 2.x muss der Recv-Callback stattdessen
  `void(const uint8_t *mac_addr, const uint8_t *data, int len)` lauten.
- **Display "verwaschen"/doppelte Bildfehler, aber NUR sobald tatsächlich
  ESP-NOW-Nachrichten empfangen werden (zweites Board eingeschaltet):**
  `onDataRecv()` lief bis dahin im WiFi/ESP-NOW-System-Task von
  Arduino-ESP32, einem ANDEREN FreeRTOS-Task als `loop()`, und zeichnete von
  dort direkt auf `canvas` bzw. sprach direkt den I2C-Bus an (`buzzerBeep()`)
  — ein klassischer Daten-Race, wenn das genau mit einem Touch-/Sende-Redraw
  in `loop()` zusammenfiel. Fix: `onDataRecv()` kopiert nur noch die Daten
  und setzt ein `volatile bool recvPending`-Flag; Zeichnen und Buzzer
  passieren ausschließlich in `loop()`, im selben Task wie alle anderen
  `canvas`-Zugriffe.
- **Empfangene Nachricht zeigt `"Hallo von 00:00:00:00:00:00"` statt der
  echten Absender-MAC im Text:** `WiFi.macAddress()` kann direkt nach
  `WiFi.mode(WIFI_STA)` bei echtem Stromlos-Start noch die Nullen-Adresse
  liefern (WLAN-Treiber intern noch nicht ganz bereit) — das betrifft dann
  den SENDER, dessen `outgoing.text` beim eigenen Boot mit der Nullen-MAC
  gefüllt wurde (die separat übertragene Absender-MAC von ESP-NOW selbst,
  `info->src_addr`, ist davon nicht betroffen und bleibt korrekt). `setup()`
  wartet deshalb nach `WiFi.mode(WIFI_STA)` bis zu 2s auf eine echte
  MAC-Adresse, bevor `outgoing.text` gefüllt wird.
- **SD-Karte während des Betriebs eingesteckt/entfernt wird nicht erkannt:**
  `SD.cardType()` liefert nach dem ersten `SD.begin()` nur den
  zwischengespeicherten Wert zurück, keine Live-Abfrage der Hardware.
  `checkSdCard()` (Stage 4) versucht deshalb alle 2s neu zu mounten, solange
  noch keine Karte gefunden wurde — vergleicht das Ergebnis mit dem zuletzt
  bekannten Zustand und zeichnet nur bei einem tatsächlichen Wechsel neu
  bzw. hängt eine Startmarke an `/espnow_log.txt` an.
- **SD-Statuszeile bleibt dauerhaft leer, obwohl die Karte laut Beep/Log
  offenbar erkannt wird:** Frühere Version von `checkSdCard()` machte bei
  JEDEM Poll-Zyklus (auch wenn die Karte bereits erfolgreich gemountet war)
  einen kompletten Remount (`SD.end()` + `sdSpi.end()` + `sdSpi.begin()` +
  `SD.begin()`) — `SD.begin()` kann bei manchen Karten spürbar lange
  blockieren, was den kompletten `loop()`-Task für diese Zeit anhielt.
  Jetzt wird nur noch beim allerersten Erkennen ein echter Mount-Versuch
  gemacht; solange die Karte als gemountet gilt, reicht ein günstiger
  Lebendigkeits-Check (Log-Datei kurz öffnen/schließen, ohne zu schreiben).
  Zusätzlich gibt `checkSdCard()` jetzt bei jedem Mount-Versuch eine
  Serial-Zeile aus (`SD.begin() -> true/false`) — beim Debuggen unbedingt
  den Serial Monitor (115200 Baud) mitlaufen lassen.
- **PN532 antwortet nicht / GetFirmwareVersion schlägt fehl:** DIP-Schalter
  auf dem Modul falsch (muss SPI = Sw1 OFF/Sw2 ON sein), oder
  `PN532_PACKBUFFSIZ`-Patch fehlt.
- **PN532 wird erkannt, aber beim Scannen einer DESFire-Karte passiert
  nichts:** Ein früherer Versuch rief zuerst `readPassiveTargetID()` (mit
  nur 10 ms Timeout) und danach zusätzlich `inListPassiveTarget()` auf.
  10 ms war zu knapp — DESFire-Karten antworten auf die
  ISO14443A-Anticollision/Select-Sequenz teils spürbar langsamer als
  einfache Mifare-Classic-Karten, und Software-SPI (statt Hardware-SPI)
  fügt zusätzliche Kommunikationslatenz hinzu, sodass der Lesevorgang
  regelmäßig in den Timeout lief. Auf 100 ms erhöht, damit kam die UID
  zwar an, aber der anschließende `inListPassiveTarget()`-Aufruf schlug an
  echter Hardware fehl (`DESFire: inListPassiveTarget() fehlgeschlagen`,
  siehe Foto-Report). Grund: `readPassiveTargetID()` und
  `inListPassiveTarget()` senden **dasselbe** native
  InListPassiveTarget-Kommando (`0x4A`) an den PN532 — der erste Aufruf
  aktiviert/selektiert die Karte bereits, ein zweiter Aufruf direkt danach
  pollt erneut nach einem Tag, während die Karte noch im aktivierten
  Zustand ist und auf eine erneute Anticollision/Select-Sequenz nicht mehr
  antwortet. **Fix:** nur noch ein einziger `inListPassiveTarget()`-Aufruf
  zur Erkennung UND Aktivierung; kein `readPassiveTargetID()` mehr im
  Poll-Pfad. Die für `inDataExchange()`-Kommandos benötigte interne
  `_inListedTag`-Variable wird dabei korrekt gesetzt (das war der
  ursprüngliche Grund, `inListPassiveTarget()` überhaupt zu brauchen). Die
  UID fürs Display kommt jetzt aus der DESFire-eigenen `GetVersion`-Antwort
  (`desfireGetVersion()`), nicht mehr aus `readPassiveTargetID()`.
- **NTP-SYNC-Button scheint nichts zu tun, obwohl WLAN-Zugangsdaten
  korrekt eingetragen sind:** `syncFromNtp()` loggte Erfolg/Fehlschlag
  bisher nur über Serial — auf dem Display war der Button-Flash (grün,
  dann grau) immer gleich, egal ob die WLAN-Verbindung, die NTP-Anfrage
  oder gar nichts fehlgeschlagen ist (oder `USE_WIFI_NTP_SYNC` schlicht
  nicht einkommentiert war). `syncFromNtp()` gibt jetzt `true`/`false`
  zurück, und der Button leuchtet bei einem Fehlschlag kurz **rot** auf,
  bevor er auf grau zurückspringt — bei Erfolg direkt grau wie gewohnt.
  Für die genaue Fehlerursache (Verbindung vs. Server nicht erreichbar)
  weiterhin den Serial Monitor mitlaufen lassen. Seit Stage 5 zusätzlich
  eine WLAN-IP-Zeile im Display (`updateWifiInfo()`, unter der UID/DESFire-
  Zusammenfassung) — die wird nach jedem NTP-SYNC-Versuch aktualisiert und
  zeigt an, ob `WiFi.begin()` überhaupt eine IP bekommen hat, auch wenn der
  anschließende NTP-Request selbst fehlschlägt.
- **WLAN/NTP funktioniert trotz mehrfacher Fixes weiterhin nicht, auch mit
  korrekt eingetragenen Zugangsdaten:** Der eigentliche Grund war banal —
  `USE_WIFI_NTP_SYNC` musste bisher per `// #define USE_WIFI_NTP_SYNC` von
  Hand in `05_pn532_spi.ino`/`main.cpp` einkommentiert werden. Diese Datei
  ist aber versioniert; bei jedem `git pull` einer neuen Version wurde die
  lokale Handänderung wieder auf "auskommentiert" zurückgesetzt, ohne dass
  es auffiel — WLAN blieb dadurch nach jedem Update erneut deaktiviert.
  **Fix:** `USE_WIFI_NTP_SYNC` wird jetzt automatisch gesetzt, sobald
  `wifi_secrets.h` existiert (`#if __has_include("wifi_secrets.h")` statt
  der manuellen `#define`-Zeile) — diese Datei ist git-ignored/lokal und
  übersteht damit jeden Pull. Es reicht, sie einmal anzulegen (Kopie von
  `wifi_secrets.example.h`, eigene Zugangsdaten eintragen); an der
  versionierten `.ino`/`main.cpp` muss dafür nichts mehr geändert werden.
- **Buttons reagieren erst nach 1-2 Sekunden gedrückt halten:** Der
  PN532-Poll ruft bei jedem 300-ms-Zyklus `nfc.inListPassiveTarget()` auf.
  Ohne Karte auf dem Leser sucht dabei **der PN532-Chip selbst** (nicht
  die Bibliothek) intern nach einer Karte, bevor er ueberhaupt antwortet —
  gesteuert vom `RFConfiguration`-Register `MxRtyPassiveActivation`, das
  werksseitig auf `0xFF` (= endlos wiederholen) steht. Das hielt `loop()`
  bei jedem Poll-Zyklus fest, sodass die Touch-Abfrage weiter unten nur
  noch alle 1–1,3 Sekunden drankam — ein Loslassen des Fingers vor dem
  nächsten `loop()`-Durchlauf wurde schlicht verpasst.
  Ein erster Versuch, das über `nfc.inListPassiveTarget(PN532_MIFARE_ISO14443A, 100)`
  (Timeout-Argument) zu fixen, ist am Build gescheitert — die hier
  gepinnte Bibliotheksversion (`adafruit/Adafruit PN532@^1.3.3`) bietet
  `inListPassiveTarget()` nur parameterlos an, keine Timeout-Überladung.
  **Tatsächlicher Fix:** `nfc.setPassiveActivationRetries(1)` einmalig in
  `pn532Begin()` (nach `SAMConfig()`) — begrenzt die internen
  Wiederholversuche des Chips selbst auf 1, sodass er ohne Karte sofort
  "nichts gefunden" zurückmeldet, statt endlos zu suchen. Der nächste
  300-ms-Poll-Zyklus versucht es einfach erneut, eine gerade erst
  aufgelegte Karte wird dadurch nicht verpasst.
- **Touch-Debug-Ausgabe / `syncFromNtp()`s "."-Fortschrittsausgabe
  verlangsamen den Sketch spürbar:** ESP32-S3-USB-CDC-`Serial` blockiert,
  sobald der interne Sendepuffer vollläuft und niemand ihn ausliest (kein
  Serial Monitor verbunden). Beide Stellen prüfen jetzt vorher
  `if (Serial)` (liefert bei USB-CDC nur `true`, wenn tatsächlich ein Host
  verbunden ist) und überspringen die Ausgabe sonst, statt zu blockieren.
- **Nach `git pull` + Neu-Flashen ist nicht erkennbar, ob wirklich die
  neueste Version läuft:** Stage 5 zeigt seit `FIRMWARE_VERSION` in der
  MAC-Adress-Zeile auf dem Display eine Versionsnummer (Datum + laufende
  Nummer, z. B. `v2026-07-28.1`) — wird bei jeder inhaltlichen Änderung an
  `05_pn532_spi.ino`/`main.cpp` hochgezählt. Steht nach dem Flashen dort
  eine ältere Nummer als erwartet, wurde entweder nicht neu gepullt oder
  das falsche Projekt/der falsche Ordner gebaut.

## 8. DESFire-Tiefenauslesung (Stage 5, `desfire.h`)

Auf Nutzerwunsch geht Stage 5 über reines UID-Lesen hinaus und versucht bei
jeder erkannten Karte einen vollständigen DESFire-Lesevorgang mit dem
**Werks-Default-Schlüssel** (16 Nullbytes):

1. `GetVersion` (Hardware-/Software-Version, UID, Batch-Nummer, Produktionswoche/-jahr)
2. `GetApplicationIDs` (Liste aller Anwendungen/AIDs auf der Karte)
3. Pro Anwendung: `SelectApplication`, dann Authentifizierung mit
   Default-Schlüssel — erst Legacy-2K3DES (Kommando `0x0A`), bei
   Fehlschlag AES-128 (Kommando `0xAA`)
4. Bei erfolgreicher Authentifizierung: `GetFileIDs`, pro Datei
   `GetFileSettings` (Typ, Kommunikationsmodus, Größe) und — wo möglich —
   `ReadData`/`ReadRecords`

Alle Details landen mit RTC-Zeitstempel in `/desfire_log.txt` auf der
SD-Karte (`desfireDeepRead()` in `05_pn532_spi.ino`/`desfire.h`); auf dem
Display steht nur eine kurze Ein-Zeilen-Zusammenfassung (z. B. `DESFire: 2
App(s), 1 authentifiziert, siehe SD-Log`).

**Bewusst begrenzter Umfang** (siehe Kopfkommentar in `desfire.h`):

- Unterstützt nur Legacy-2K3DES- und AES-128-Authentifizierung mit einem
  16-Byte-Schlüssel — **kein** 3K3DES, **kein** EV2-Secure-Messaging.
- Dateien im **Enciphered**-Kommunikationsmodus werden erkannt, aber
  **nicht entschlüsselt** (im Log als "Enciphered, wird nicht gelesen"
  markiert).
- Dateien im **MACed**-Modus werden gelesen, der angehängte MAC/CMAC wird
  **nicht geprüft** — die Rohdaten selbst sind bei MACed unverschlüsselt
  übertragen, nur zusätzlich signiert.
- Der aus RndA/RndB abgeleitete Sitzungsschlüssel wird berechnet, aber
  nicht weiterverwendet (keine Secure-Messaging-Kommandos danach nötig).
- **Dieser Code wurde an keiner echten DESFire-Karte getestet** (kein
  Testgerät verfügbar) — er basiert auf der öffentlich dokumentierten
  NXP-Spezifikation (ISO/IEC 9798-2 3-Pass-Mutual-Authentication). Schlägt
  die Authentifizierung fehl, ist die wahrscheinlichste Erklärung, dass
  die Karte (wie bei den meisten Karten aus dem echten Einsatz) nicht mehr
  die Werksschlüssel hat — das ist dann korrektes Verhalten, kein Bug.
  Serial-Ausgaben an jedem Protokollschritt helfen, einen echten Bug von
  "Karte hat andere Schlüssel" zu unterscheiden.

**Build-Fehler `undefined reference to mbedtls_des3_init` (u. ä.):**
Trat früher auf, weil `desfire.h` für die 2K3DES-Authentifizierung
`mbedtls_des3_*` nutzte. Bei `framework=arduino` wird die mbedtls-Komponente
aber als fertig **vorkompiliertes Binary** mit dem Framework-Paket
ausgeliefert, nicht aus Quellcode neu gebaut — eine `CONFIG_MBEDTLS_DES_C=y`
in `sdkconfig.defaults` (wie in einer früheren Version dieser Datei
empfohlen) hat darauf **keine Wirkung**, das Linken gegen `mbedtls_des3_*`
schlägt unabhängig davon immer fehl, weil DES/2K3DES schlicht nicht Teil des
vorkompilierten Binaries ist (`mbedtls_aes_*` für die AES-Authentifizierung
dagegen schon). Deshalb implementiert `desfire.h` DES/2K3DES inzwischen
**komplett selbst** (Standard-FIPS-46-3-Tabellen und -Algorithmus, siehe
Kopfkommentar der Datei) und braucht `mbedtls/des.h` gar nicht mehr — der
Fehler kann damit weder in PlatformIO noch in der Arduino IDE mehr
auftreten, da beide Toolchains dieselbe `desfire.h` verwenden.

## 9. Hochformat-Drehung (Stage 5, `DISPLAY_ROTATION`)

Auf Nutzerwunsch läuft Stage 5 im Hochformat (480 breit × 800 hoch) statt
im ursprünglichen Querformat (800×480) — **nur** Stage 5, die Stufen 1-4
bleiben unverändert im Querformat.

Das Panel selbst ist als 800×480 fest verdrahtet (feste RGB-Timings in
`rgb_panel.h`, `h_res`/`v_res` = `LCD_WIDTH`/`LCD_HEIGHT`) — die Drehung
ändert diese Hardware-Timings NICHT, sondern passiert rein als
Koordinatentransformation in `canvas.setRotation(DISPLAY_ROTATION)`
(direkt nach `canvas.setBuffer()` in `setup()`). Ab da liefern
`canvas.width()`/`canvas.height()` die gedrehten Maße (480/800), und alle
Layout-Konstanten (Buttons, `*_INFO_Y`-Bereichsgrenzen) sind für dieses
480×800-System gebaut — eine Spalte Buttons statt des vorherigen
Zwei-Reihen-Rasters, da bei 480px Breite nicht mehr genug Platz für zwei
oder drei Buttons nebeneinander ist.

**Zwei Dinge, die beim Testen an echter Hardware geprüft werden müssen**
(am Schreibtisch nicht verifizierbar):

- **Drehrichtung:** `DISPLAY_ROTATION = 1` (90° im Uhrzeigersinn) ist ein
  Startwert — ob das am montierten Panel wirklich "0/0 unten links"
  ergibt oder das Bild auf dem Kopf steht, hängt von der physischen
  Montage ab. Falls falsch: auf `3` (270°, = 90° gegen den Uhrzeigersinn)
  ändern.
- **Touch-Koordinaten:** Der GT911-Touch-Chip kennt `canvas.setRotation()`
  nicht — er liefert weiterhin rohe Koordinaten im physischen 800×480-
  Raster (siehe Kopfkommentar in `touch_standalone.h`, "reine
  Identitätsabbildung"). Die neue Funktion `rotateTouchToLogical()`
  rechnet das per Adafruit_GFX-/LovyanGFX-Rotationsformel (aus
  `GFXcanvas::drawPixel()` zurückgerechnet) in die gedrehte 480×800-
  Koordinate um, **bevor** die Button-Trefferprüfung (`inside()`) läuft.
  Sie liest denselben `DISPLAY_ROTATION`-Wert wie `canvas.setRotation()`
  — beide bleiben also automatisch synchron, wenn der Wert oben
  geändert wird. Reagieren Buttons trotz sichtbar korrekt gedrehtem Bild
  nicht (oder an der falschen Stelle), ist das ein Hinweis, dass die
  Rotationsformel für den gewählten `DISPLAY_ROTATION`-Wert nicht zur
  tatsächlichen LovyanGFX-Konvention passt — bitte melden, dann wird die
  Formel korrigiert.

## 10. DESFire-Schreibfunktionen (Stage 5, 7 zusätzliche Buttons)

Auf Nutzerwunsch kann Stage 5 jetzt auch schreibend auf eine DESFire-Karte
zugreifen: eigene Applikation mit Guthaben-Datei anlegen/löschen,
Guthaben buchen/nutzen/abfragen, und den Werks-Default-Schlüssel gegen
einen eigenen Schlüssel tauschen (und zurück). Alle 7 Buttons rechts
unten im Hochformat-Layout (siehe Abschnitt 9), alle Parameter fest im
Code (`05_pn532_spi.ino`), da das Panel keine Tastatur hat:

| Konstante | Wert | Bedeutung |
|---|---|---|
| `CUSTOM_AID` | `0x123456` | AID der selbst angelegten Applikation |
| `CUSTOM_KEY` | 16 zufällige Byte (`openssl rand -hex 16`) | Ersetzt Key 0 bei "MASTER-PW SETZEN" |
| `VALUE_FILE_NO` | `0` | Einzige Datei in der App, ein Value File |
| `VALUE_LOWER_LIMIT` / `_UPPER_LIMIT` | `0` / `1000000` | Grenzen des Guthabens |
| `CREDIT_DEBIT_AMOUNT` | `100` | Fester Betrag pro Tastendruck |

**Aktions-Fenster statt Sofortausführung:** Jeder der 7 Buttons öffnet ein
eigenes Fenster (`DesfireModalState`, `drawDesfireModalConfirm()`/
`drawDesfireModalResult()`) — erst eine Bestätigung mit **ABBRECHEN**/
**AUSFÜHREN**, nach der Ausführung das Ergebnis mit **SCHLIESSEN**-Button.
Grund: der automatische Lese-Scan (Abschnitt 8) lief anfangs weiter im
Hintergrund und überschrieb das Ergebnis eines gerade gedrückten Buttons
fast sofort mit seiner eigenen generischen Zusammenfassung — sichtbar war
dann nur noch der Piepton, ohne erkennbaren Effekt. Ein erster Fix (Auto-
Scan für einige Sekunden nach jedem Tastendruck pausieren) kaschierte das
nur; jetzt pausiert der komplette Hintergrund-Block (siehe `loop()`) für
die gesamte Dauer, in der das Fenster offen ist — keine Race Condition
mehr, das PN532-Modul bleibt dabei durchgehend aktiv, nur eben ohne
Hintergrund-Polling währenddessen.

**Buttons:**

- **MASTER-PW SETZEN / AUF STANDARD** — authentifiziert mit dem jeweils
  *aktuell gültigen* Schlüssel (`desfireAuthEitherKey()` probiert erst
  Default, dann Custom) und ändert Key 0 per `ChangeKey` auf den anderen
  Wert. Wirkt **sowohl auf PICC-Ebene** (AID `000000`, betrifft die
  **gesamte Karte**) **als auch** — falls schon vorhanden — auf die
  eigene Applikation, in **einem** Tastendruck. Existiert die App noch
  nicht, wird nur die PICC-Ebene gesichert; ein erneuter Tastendruck nach
  "APP ERSTELLEN" sichert dann auch die App nachträglich.
- **APP ERSTELLEN** — `CreateApplication(CUSTOM_AID)` auf PICC-Ebene,
  danach `CreateValueFile` (Guthaben startet bei 0) in der neuen App.
- **APP LOESCHEN** — `DeleteApplication(CUSTOM_AID)`, muss laut
  DESFire-Vorgabe auf PICC-Ebene authentifiziert aufgerufen werden.
- **GUTHABEN BUCHEN / NUTZEN** — `Credit`/`Debit` + `CommitTransaction`
  (ohne Commit werden Buchungen beim nächsten Kommando verworfen). Bei
  "NUTZEN": die **Karte selbst** lehnt ab, wenn das Guthaben dadurch unter
  `VALUE_LOWER_LIMIT` (0) fallen würde (Status meist `BOUNDARY_ERROR`) —
  keine eigene Guthaben-Prüfung nötig/implementiert.
- **GUTHABEN ABFRAGEN** — `GetValue`, Ergebnis in der Zusammenfassungszeile.

**Sicherheitshinweis ChangeKey (bitte lesen, bevor an einer echten Karte
getestet wird):** Das Kryptogramm enthält eine CRC (CRC-A/CRC16 bei
2K3DES, CRC-32 bei AES), die die **Karte selbst** nach dem Entschlüsseln
prüft. Stimmen IV, Byte-Layout oder CRC nicht exakt mit der
NXP-Spezifikation überein, verwirft die Karte das Kommando (Status
ungleich `0x00`) — der Schlüssel bleibt dann **unverändert**, es entsteht
kein kaputter/unbekannter Schlüssel-Zustand. Trotzdem: dieser Code wurde
**nicht an echter DESFire-Hardware getestet**. Implementiert ist
außerdem **nur** der Fall "eigenen, gerade authentifizierten Schlüssel
ändern" — kein Wechsel eines *anderen* als des authentifizierten
Schlüssels (dafür bräuchte es die XOR-Verknüpfung von Alt-/Neuschlüssel
im Kryptogramm, hier nirgends gebraucht).

**Bewusst weggelassen:** CMAC/Secure-Messaging für die Dateikommunikation
(Value File läuft im Plain-Modus), Schlüssel-Eingabe über die UI (alles
hardcoded), allgemeiner ChangeKey für fremde Schlüssel, mehr als 1
Schlüssel/Applikation.

## 11. Log-Fenster + SD-Tageslog + EINSTELLUNGEN-Untermenü (Stage 5)

Auf Nutzerwunsch gibt es jetzt zwei Wege, denselben Log-Inhalt zu lesen,
statt nur über den Serial Monitor:

**Log-Fenster (`LOG ANZEIGEN`-Button, `drawLogView()`):** Ein fast
bildschirmfüllendes, eigenes Fenster (kein permanent sichtbares Panel
mehr — das war zu klein/schwer lesbar) mit deutlich größerer Schrift
(`textSize(1.5)` statt `1`) und entsprechend mehr Platz pro Zeile. **HOCH**/
**RUNTER** blättern durch die letzten `LOG_LINES` (200) Zeilen im
Ringpuffer, **SCHLIESSEN** kehrt zum Hauptbildschirm zurück. Aktualisiert
sich automatisch bei jeder neuen Meldung, solange es offen ist (live
mitlesbar während einer laufenden Aktion).

**SD-Tageslog (`logToSd()`):** Jede Log-Zeile landet zusätzlich — mit
RTC-Zeitstempel — in einer tagesaktuellen Datei auf der SD-Karte, z. B.
`/log_2026-07-28.txt`. Da der Dateiname das Datum enthält, entsteht beim
Datumswechsel automatisch eine neue Datei, ohne eigene Rotationslogik.
No-op, solange keine Karte gemountet ist; sobald eine eingesteckt wird,
protokolliert `checkSdCard()`s nächster Durchlauf ab da weiter.

**Umsetzung:** `logMsg(fmt, ...)` (in `05_pn532_spi.ino`/`main.cpp`, noch
vor `#include "desfire.h"` definiert) ersetzt praktisch jeden bisherigen
`Serial.println()`/`Serial.printf()`-Aufruf im gesamten Sketch **und** in
`desfire.h` — schreibt weiterhin nach Serial (nur falls ein Monitor
verbunden ist), hängt die Zeile an den Ringpuffer **und** an `logToSd()`
an. Da `desfire.h` `logMsg()` schon sehr früh in der Datei braucht,
`canvas`/`sdMounted`/`formatTimestamp()` zu dem Zeitpunkt aber noch nicht
deklariert sind, gibt es dafür Vorwärtsdeklarationen (`void
drawLogView();` und `void logToSd(const char *line);`) ganz am Anfang der
Datei — die tatsächlichen Funktionskörper folgen viel später (siehe auch
Kommentar bei `desfireRunModalAction()` zum selben Thema).

**Platz geschaffen fürs Log-Fenster:** NTP-SYNC, der WLAN/ESP-NOW-
Umschalter und BACKLIGHT +/- sind aus dem Hauptbildschirm in ein
**EINSTELLUNGEN**-Untermenü gewandert (ein Button auf dem Hauptbildschirm
öffnet es). Anders als beim DESFire-Aktions-Fenster gibt es dort **keinen**
Bestätigungsschritt — diese vier Aktionen sind unkritisch und sofort
rückgängig machbar, anders als die DESFire-Schreibkommandos.

## 12. Ausblick — nicht Teil dieser fünf Stages

- Kombiniertes Gesamtprojekt, das alle Subsysteme gleichzeitig nutzt.
- CMAC-Schutz/Enciphered-Kommunikation nach der Authentifizierung,
  EV2-Secure-Messaging, 3K3DES, ChangeKey für einen anderen als den
  authentifizierten Schlüssel — deutlich größerer Umfang als die
  Lese-/Schreib-Funktionalität in Abschnitt 8/10.
