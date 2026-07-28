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
- **Vormerkung fürs Lesen/Schreiben größerer Dateien (noch nicht
  implementiert, aktuell nur `CreateValueFile`/`Credit`/`Debit`/
  `GetValue` mit winzigen Kommandos):** Ein unabhängiges Tutorial
  (AndroidCrypto, "ESP32 + Adafruit_PN532 + DESFire EVx", Medium,
  Nov. 2025 — nutzt dieselben zwei Pflicht-Patches an der
  Adafruit-Bibliothek wie wir, siehe oben) beschreibt ein zusätzliches
  PN532-Limit: einzelne `inDataExchange()`-Aufrufe mit mehr als ~223 Byte
  Nutzdaten liefern trotz auf 255 gepatchtem `PN532_PACKBUFFSIZ` keinen
  Fehler, sondern scheinbar zufälligen PN532-internen Speicherinhalt
  zurück. Lösung dort: größere Schreib-/Lesevorgänge selbst in Stücke von
  z. B. 210 Byte aufteilen (mehrere `inDataExchange()`-Aufrufe statt
  einem). Betrifft uns aktuell nicht (alle unsere Kommandos sind winzig),
  aber unbedingt beachten, sobald hier mal größere Standard-Data-Files
  gelesen/geschrieben werden sollen.

## 8. DESFire-Tiefenauslesung (Stage 5, `desfire.h`)

Auf Nutzerwunsch geht Stage 5 über reines UID-Lesen hinaus und führt einen
vollständigen DESFire-Lesevorgang mit dem **Werks-Default-Schlüssel**
(16 Nullbytes) durch. **Ausgelöst über den KARTEN-INFO-Button** (siehe
Abschnitt 10) — läuft NICHT mehr automatisch im Hintergrund bei jeder
erkannten Karte (Nutzerwunsch: der PN532 soll nur nach einem bewussten
Tastendruck aktiv scannen, nicht durchgehend):

1. `GetVersion` (Hardware-/Software-Version, UID, Batch-Nummer, Produktionswoche/-jahr)
2. `GetApplicationIDs` (Liste aller Anwendungen/AIDs auf der Karte)
3. Pro Anwendung: `SelectApplication`, dann Authentifizierung mit
   Default-Schlüssel — erst 2K3DES (Kommando `0x1A`, siehe Abschnitt 12
   zur Begründung, warum nicht `0x0A`), bei Fehlschlag AES-128 (Kommando
   `0xAA`)
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
  "Karte hat andere Schlüssel" zu unterscheiden. **Wichtig zur
  Unterscheidung** (siehe Abschnitt 12 für die Herleitung anhand eines
  echten Hardware-Logs): Lehnt die Karte schon in Schritt 2 mit Status
  `0xAE` ab, ist der Schlüssel falsch. Antwortet die Karte dagegen mit
  Status `0x00` und **nur** die eigene RndA-Rückprüfung schlägt fehl, war
  der Schlüssel korrekt — dann liegt es an einem einzelnen
  Übertragungsfehler (wird seit Abschnitt 12 automatisch bis zu 3x erneut
  versucht), nicht am Schlüssel.

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

## 10. DESFire-Schreibfunktionen (Stage 5, 8 Aktions-Buttons)

Auf Nutzerwunsch kann Stage 5 jetzt auch schreibend auf eine DESFire-Karte
zugreifen: eigene Applikation mit Guthaben-Datei anlegen/löschen,
Guthaben buchen/nutzen/abfragen, Karteninformationen anzeigen, und den
Werks-Default-Schlüssel gegen einen eigenen Schlüssel tauschen (und
zurück). Alle 8 Buttons rechts unten im Hochformat-Layout (siehe
Abschnitt 9), alle Parameter fest im Code (`05_pn532_spi.ino`), da das
Panel keine Tastatur hat:

| Konstante | Wert | Bedeutung |
|---|---|---|
| `CUSTOM_AID` | `0x123456` | AID der selbst angelegten Applikation |
| `CUSTOM_KEY` | 16 zufällige Byte (`openssl rand -hex 16`) | Ersetzt Key 0 bei "MASTER-PW SETZEN" |
| `VALUE_FILE_NO` | `0` | Einzige Datei in der App, ein Value File |
| `VALUE_LOWER_LIMIT` / `_UPPER_LIMIT` | `0` / `1000000` | Grenzen des Guthabens |
| `CREDIT_DEBIT_AMOUNT` | `100` | Fester Betrag pro Tastendruck |

**Aktions-Fenster mit aktivem Warten auf die Karte (Nutzerwunsch,
mehrfach überarbeitet):** Jeder Aktions-Button öffnet ein eigenes Fenster
(`DesfireModalState`). Für die kritischen/schreibenden Aktionen erst eine
Bestätigung mit **ABBRECHEN**/**AUSFÜHREN** (`drawDesfireModalConfirm()`);
für das rein lesende KARTEN INFO entfällt dieser Schritt. Danach wechselt
das Fenster in einen **aktiven Wartezustand** (`MODAL_WAITING_CARD`,
`drawDesfireModalWaiting()`): "Karte jetzt auflegen" wird angezeigt, der
PN532 wird jetzt erst periodisch abgefragt (vorher lief das Polling
durchgehend im Hintergrund, auch ohne dass irgendeine Aktion gewünscht
war — auf Nutzerwunsch entfernt, das Modul soll nicht mehr staendig aktiv
scannen). Sobald eine Karte erkannt wird, wartet der Code eine kurze
Beruhigungspause (`DESFIRE_CARD_SETTLE_MS`, 300 ms) und führt dann
automatisch die Aktion aus — kein zusätzlicher Tastendruck nötig. Danach
das Ergebnis mit **SCHLIESSEN**-Button (`drawDesfireModalResult()`).
Gepiept wird ausschließlich ganz am Ende (`desfireOpFinish()`), nie schon
beim bloßen Erkennen der Karte.

**Buttons:**

- **KARTEN INFO** — rein lesend, kein Bestätigungsschritt: liest UID +
  DESFire-Kurzinfo der aufgelegten Karte (`desfireDeepRead()`, wie zuvor
  automatisch im Hintergrund, jetzt bewusst ausgelöst). Ergebnisfenster
  zeigt UID-Zeile + Kurzzusammenfassung; Details weiterhin in
  `/desfire_log.txt`. `drawDesfireModalResult()` ist bewusst so gebaut,
  dass sich hier später leicht weitere Karteninformationen ergänzen
  lassen (ein zusätzlicher `printWrapped()`-Aufruf pro neuer Zeile).
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
  (ohne Commit werden Buchungen beim nächsten Kommando verworfen), danach
  automatisch `GetValue` für den neuen Kontostand (Nutzerwunsch) —
  Ergebnisfenster zeigt z. B. "+100 gebucht, neuer Stand: 300" statt nur
  "gebucht". Bei "NUTZEN": die **Karte selbst** lehnt ab, wenn das
  Guthaben dadurch unter `VALUE_LOWER_LIMIT` (0) fallen würde (Status
  meist `BOUNDARY_ERROR`) — keine eigene Guthaben-Prüfung nötig/
  implementiert.
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

**Bugfix: lange Log-Zeilen liefen mit der nächsten Zeile ineinander.**
`drawLogView()` zeichnete ursprünglich jeden Log-Eintrag mit `canvas.print()`
an eine feste Y-Position (`LOGVIEW_LINE_H` = 16px pro Eintrag), unabhängig
davon, ob LovyanGFX den Text intern über mehrere Bildschirmzeilen umbrach —
Einträge nahe der 72-Zeichen-Obergrenze (`LOG_LINE_LEN`) sind bei
`textSize(1.5)` im 440px breiten Textbereich fast immer zweizeilig, die
zweite Zeile überschrieb dann den nächsten Eintrag (an echter Hardware
sichtbar geworden). **Fix:** `drawLogView()` nutzt jetzt denselben
`printWrapped()`-Zeilenumbruch-Helfer wie das DESFire-Aktions-Fenster, misst
die tatsächlich benötigte Zeilenzahl pro Eintrag und rückt den Cursor
entsprechend weiter — die Zahl der pro Bildschirm sichtbaren Einträge
variiert dadurch (kürzere Meldungen passen mehr rein), `HOCH`/`RUNTER`
bleiben aber weiterhin pro **Eintrag** (nicht pro Bildschirmzeile). Da
`printWrapped()` dadurch schon vor seiner eigentlichen Definition
gebraucht wird, gibt es dafür — wie bei `drawLogView()`/`logToSd()` selbst
— eine Vorwärtsdeklaration ganz am Anfang der Datei (PlatformIOs
`main.cpp` hat keine automatische Prototyp-Generierung wie die Arduino
IDE, siehe Abschnitt 7).

## 12. Erste echte Hardware-Diagnose: Auth erreicht Status 0x00, RndA-Prüfung schlägt trotzdem fehl

Erstes reales SD-Log von echter Hardware (`MASTER-PW SETZEN`) zeigte:
`desfireAuthDes3: RndA-Rueckpruefung fehlgeschlagen`, danach beim Versuch
mit dem Custom-Schlüssel `Schritt 2 unerwartet (status=0xAE)`. Auf den
ersten Blick sieht das nach "falscher Schlüssel" aus (so auch die
bisherige Logmeldung), ist es hier aber **nicht**:

- Bei einem wirklich falschen Schlüssel liefert die Entschlüsselung von
  `EncAB` auf der Karte mit **falschem** Schlüssel praktisch immer
  Pseudozufall — die Karte lehnt dann schon in Schritt 2 mit Status `0xAE`
  ab, **bevor** überhaupt eine `EncRndA`-Antwort verschickt wird (exakt das
  Verhalten, das der Custom-Schlüssel-Versuch im Log zeigt).
- Der Werks-Default-Schlüssel-Versuch (16 Nullbytes) bekam dagegen Status
  `0x00` in Schritt 2 — die Karte hat das Kryptogramm also akzeptiert, der
  Schlüssel war korrekt. Erst die **eigene** Entschlüsselung/Prüfung der
  `EncRndA`-Antwort schlug fehl.
- Die IV-Verkettung in `desfireAuthDes3()`/`desfireAuthAes()` (letzter
  gesendeter Chiffretext-Block als IV für die finale Entschlüsselung) wurde
  gegen die ISO/IEC-9798-2-Spezifikation und mehrere quelloffene
  Referenzimplementierungen nachgerechnet und ist korrekt — **kein**
  Krypto-/IV-Bug.

Bleibt als Erklärung ein einzelner Übertragungsfehler auf der
Luftschnittstelle (Bit-/Byte-Fehler in der Rückantwort der Karte) —
plausibel angesichts der schon früher in diesem Projekt beobachteten
PN532-Timing-/RF-Empfindlichkeiten (siehe Abschnitt 7). **Fix:** Sowohl
`desfireAuthDes3()` als auch `desfireAuthAes()` wiederholen jetzt bis zu
3x den **kompletten** Challenge-Response-Ablauf (neues RndA, neue
Kommandos — der bisherige Austausch lässt sich nicht "fortsetzen"), aber
**nur** für genau diesen Fall (Status `0x00` in Schritt 2, aber
fehlgeschlagene eigene RndA-Prüfung). Ein echter Status-Fehler (`0xAE` o.
ä.) in Schritt 1 oder 2 bricht weiterhin sofort ohne Retry ab — das ist
eine echte Ablehnung durch die Karte, kein Übertragungsproblem, und ein
erneuter Versuch würde dort nur unnötig Zeit kosten. Die Logmeldung bei
endgültigem Fehlschlag weist jetzt außerdem explizit darauf hin, dass der
Schlüssel laut Karten-Status korrekt war.

**Nachtrag: Retry allein hat es NICHT behoben — echter Krypto-Bug
gefunden.** Ein zweites reales Log (mit dem Retry-Fix bereits aktiv, echte
EV3-Karte, per TagInfo als Original-NXP-Chip verifiziert) zeigte: **alle
3 unabhängigen Versuche** scheitern identisch (Status `0x00` in Schritt 2,
eigene RndA-Prüfung schlägt fehl). Bei einem echten Übertragungsfehler
(zufälliger Bitfehler auf der Luftschnittstelle) wäre es praktisch
ausgeschlossen, dass 3 komplett unabhängige Durchläufe (jeweils frisches
`RndA`, frisches `RndB` von der Karte) exakt gleich scheitern — das
beweist einen **deterministischen Bug**, keinen Zufallsfehler.

Byte-für-byte-Vergleich der eigenen Implementierung gegen den
tatsächlichen Quellcode von libfreefare (`mifare_desfire.c`/
`mifare_desfire_crypto.c`, direkt von GitHub geladen und gelesen, nicht
nur zusammengefasst) fand die Ursache: Die NXP-Legacy-Authentifizierung
(Kommando `0x0A`) verschlüsselt das PCD→PICC-Kryptogramm (`RndA||RndB'`)
**nicht** mit der normalen CBC-Verschlüsselungsfunktion, sondern — eine
dokumentierte Eigenheit dieses alten Protokolls — mit der
**Entschlüsselungsfunktion** des Blockchiffres (libfreefare übergibt dort
explizit `MCO_DECYPHER` statt `MCO_ENCYPHER`, aber **nur** für
`AUTHENTICATE_LEGACY`; AES/ISO-Authentifizierung nutzt an derselben Stelle
ganz normal die Verschlüsselungsfunktion). Der eigene Code nutzte bislang
überall die normale Verschlüsselungsfunktion.

**Warum das beim Werks-Default-Schlüssel (16 Nullbytes) unsichtbar
blieb:** Rechnerisch nachgeprüft (Python, `pycryptodome`) — bei einem
DES-Schlüssel aus lauter Nullbytes sind Ver- und Entschlüsselungsfunktion
**identisch** (jede DES-Rundenschlüsselfolge ist bei einem Nullschlüssel
palindromisch, siehe `DES_SHIFTS`-Rotation auf einem bereits-Null-Register
in `desfire.h`). Der Bug war beim Testen mit dem Werksschlüssel deshalb
mathematisch wirkungslos — **hätte** aber bei `CUSTOM_KEY` (der echte,
zufällige 16-Byte-Schlüssel für "MASTER-PW SETZEN") zugeschlagen, sobald
die Karte diesen Schlüssel tatsächlich trägt. **Fix:** neue Funktion
`desfireDes3CbcSendLegacy()` in `desfire.h`, die exakt diese Eigenheit
nachbildet (CBC-Struktur, aber mit der Entschlüsselungs-Chiffre als
Primitiv), nur für den betroffenen Schritt in `desfireAuthDes3()`.

**Das erklärt bisher aber NICHT, warum der Werks-Default-Schlüssel selbst
scheitert** (dort ist der Fix ja nachweislich wirkungslos, s. o.) — nach
erschöpfender Protokoll-/Krypto-Analyse (inkl. dieses Fixes) bleibt das
weiterhin ungeklärt. Um beim nächsten Fehlschlag echte Daten statt nur
Erfolg/Misserfolg zu haben, loggen `desfireAuthDes3()`/`desfireAuthAes()`
bei jedem fehlgeschlagenen Versuch jetzt zusätzlich die rohen
Zwischenwerte als Hex (`RndB`, `RndA`, IV für Schritt 4, `EncRndAResp`,
berechnetes vs. erwartetes `RndA'`) über die neue `logHex()`-Hilfsfunktion
— damit sich beim nächsten Fehlschlag nachrechnen lässt, ob die Abweichung
zufällig aussieht oder ein erkennbares Muster hat.

**Nachtrag 2:** Erstes reales Hex-Dump-Log ausgewertet (von Hand gegen
`pycryptodome` nachgerechnet) — mit den bis dahin geloggten Werten
(`RndB`, `RndA`, `EncAB[8:16]`, `EncRndAResp`) ließ sich keine der
naheliegenden IV-/Reihenfolge-Varianten reproduzieren, die zum von der
Karte erwarteten `RndA'` passt. Zwei mögliche Gründe: entweder fehlten
noch die Werte `EncRndB` (Kartenantwort aus Schritt 1) und `EncAB[0:8]`
(erster gesendeter Block) für einen vollständigen Test, oder es lag an
Übertragungsfehlern beim Abtippen von Werten aus einem Foto (Handy-Foto
eines gedrehten Displays, z. B. leicht zu verwechselnde Ziffern wie `0`/`D`
oder `6`/`G`). Beide jetzt behoben: `logHex()`-Diagnose ergänzt um
`EncRndB` und `EncAB[0:8]` (volle Rohdaten für alle 4 Kryptoschritte
verfügbar); außerdem gilt ab jetzt: **Hex-Diagnose bitte immer aus der
Textdatei auf der SD-Karte kopieren, nicht von einem Display-Foto
abtippen** — die Textdatei ist fehlerfrei kopierbar, ein Foto eines um
90° gedrehten Displays ist es nicht.

**WLAN-Diagnose ergänzt:** `logWifiAttemptResult()` (neue Funktion,
aufgerufen nach jedem `wifiMulti.run()`-Versuch) loggt bei Erfolg SSID/
RSSI/IP, bei Fehlschlag zusätzlich alle aus `wifi_secrets.h` konfigurierten
SSIDs UND das Ergebnis eines `WiFi.scanNetworks()` (alle tatsächlich in
Reichweite befindlichen Netzwerke) — damit sich auf einen Blick sehen
lässt, ob z. B. ein Tippfehler in der SSID vorliegt oder das Zielnetzwerk
schlicht nicht in Reichweite ist (oder ein 5-GHz-only-Netzwerk, das der
ESP32 grundsätzlich nicht sehen kann). Passwörter werden dabei nie
geloggt. Beim Setup wird zusätzlich jede registrierte SSID einzeln
geloggt (vorher nur die Anzahl) — hilft zu prüfen, ob `wifi_secrets.h`
korrekt eingelesen wurde.

**Bugfix: veraltete Log-Meldung "USE_WIFI_NTP_SYNC einkommentieren".**
Diese Meldung stammt noch aus der Zeit VOR dem `__has_include`-Fix (siehe
oben) und beschrieb einen Mechanismus, der gar nicht mehr existiert — seit
dem Fix entscheidet einzig und allein, ob `wifi_secrets.h` beim
**Kompilieren** existiert, nichts wird mehr ein- oder auskommentiert.
Beide Meldungen (NTP-SYNC-Button und WLAN/ESP-NOW-Umschalter) korrigiert:
weisen jetzt korrekt darauf hin, dass entweder `wifi_secrets.h` fehlt
(Vorlage: `wifi_secrets.example.h`) oder seit dem Anlegen nicht neu
kompiliert wurde. **Wichtig:** Das ist ein reiner
`#if __has_include(...)`-Compile-Zeit-Schalter — nur HOCHLADEN reicht
nicht, es muss ein ECHTER Neu-Build sein (in der Arduino IDE reicht ein
normaler Upload, der kompiliert automatisch neu; in PlatformIO im Zweifel
"Clean" vor "Build/Upload", falls trotz vorhandener `wifi_secrets.h`
weiterhin diese Meldung erscheint).

**Nachtrag 3: Auch mit exaktem Text-Log (keine Foto-Abtippfehler mehr)
weiterhin kein Treffer.** Ein sauberes, aus der SD-Textdatei kopiertes Log
(alle 4 Kryptowerte pro Versuch: `EncRndB`, `RndB`, `RndA`, `EncAB[0:8]`,
`EncAB[8:16]`, `EncRndAResp`, berechnetes/erwartetes `RndA'`) wurde
vollständig gegen `pycryptodome` nachgerechnet. Beide Sanity-Checks
bestanden (eigene `RndB`-Entschlüsselung und eigene `RndA'`-Berechnung
reproduzierbar — die geloggten Werte UND die Übertragung sind also
korrekt). Eine **erschöpfende** Suche über alle plausiblen IV-Kandidaten
(`0`, `EncRndB`, `EncAB[0:8]`, `EncAB[8:16]`, `RndA`, `RndB`, rotiertes
`RndB`) × beide XOR-Reihenfolgen (XOR-vor-Chiffre wie beim Legacy-
Sende-Schritt / Chiffre-dann-XOR wie beim normalen CBC-Empfang) × alle
plausiblen Zielwerte (`RndA` rotiert links/rechts/unrotiert, `RndB`
rotiert/unrotiert) fand **keine einzige Kombination**, die zum tatsächlich
erwarteten Wert passt — bei allen 3 unabhängigen Versuchen im Log.

Das schließt praktisch jede denkbare simple IV-/Verkettungs-/
Reihenfolge-Variante aus. Gleichzeitig spricht die Schrittfolge im Log
GEGEN einen falschen Schlüssel: Status `0x00` in Schritt 2 (Karte
akzeptiert das Kryptogramm) UND der sofortige `0xAE`-Fehlschlag von
`desfireAuthAes` bereits in Schritt 1 (Karte lehnt Schlüssel 0 für den
AES-Befehlstyp direkt ab, ohne ueberhaupt eine Challenge zu stellen —
das bestätigt unabhängig, dass Schlüssel 0 tatsächlich als
Legacy-2TDEA-Schlüssel konfiguriert ist, passend zu unserer Annahme).

**Ehrlicher Stand:** Die Ursache liegt an dieser Stelle außerhalb dessen,
was sich per Log-Analyse und Nachrechnen ohne echten Zugriff auf die
Karten-Gegenseite weiter eingrenzen lässt. Denkbare Erklärungen, die sich
von hier aus nicht mehr unterscheiden lassen: (a) eine PN532-/
Adafruit-Bibliotheks-Eigenheit spezifisch für dieses rohe
`inDataExchange()`-Antwort-Muster (Authenticate nutzt bewusst NICHT
`desfireTransceive()`, siehe Kopfkommentar dort), (b) eine EV3-spezifische
Abweichung vom in libfreefare (primär EV0/EV1) implementierten Ablauf, die
über die bereits geprüfte Verschlüsselungs-vs-Entschlüsselungs-Eigenheit
hinausgeht. **Empfehlung, falls weiter benötigt:** unabhängige
Gegenprobe mit einem PC-seitigen Werkzeug (z. B. ein zweiter PN532/
ACR122U an einem PC mit `libnfc`/`libfreefare`, oder ein Python-Skript mit
`nfcpy`) gegen dieselbe physische Karte — das würde eindeutig trennen, ob
das Problem im eigenen Code oder in einer Karten-/Chip-Eigenheit liegt,
die keine reine Log-Analyse mehr aufdecken kann.

**Nachtrag 4 — GELÖST (per unabhängiger Gegenprobe mit einem iCopy X /
Proxmark3): Kommando 0x0A war falsch, richtig ist 0x1A.** Genau die oben
empfohlene Gegenprobe wurde durchgeführt: ein iCopy X (Proxmark3-
iceman-Firmware) mit `hf mfdes auth -n 0 -t 2tdea -k
00000000000000000000000000000000` gegen dieselbe physische Karte —
**Ergebnis: "PICC selected and authenticated succesfully"**. Das beweist
zweifelsfrei: der Werks-Default-Schlüssel (16 Nullbytes) ist korrekt,
unser eigener Code hatte tatsächlich einen Bug.

Der tatsächliche Proxmark3-Quellcode (RfidResearchGroup/proxmark3,
`client/src/mifare/desfirecore.c`, Funktion `DesfireAuthenticateEV1()`)
wurde direkt von GitHub geladen und gelesen (nicht nur zusammengefasst).
Zentraler Fund: Für einen 2K3DES-artigen Schlüssel sendet Proxmark3 im
"EV1"-Sicherheitskanal (den es für diese Karte automatisch wählt, siehe
`Secure channel: ev1` in der obigen Ausgabe) **Kommando `0x1A`
(`AUTHENTICATE_ISO`), NICHT `0x0A` (`AUTHENTICATE`/"Legacy")**:

```c
if (secureChannel == DACEV1) {
    if (dctx->keyType == T_AES)
        subcommand = MFDES_AUTHENTICATE_AES;
    else
        subcommand = MFDES_AUTHENTICATE_ISO;   // 0x1A, nicht 0x0A!
}
```

Unser Code (und die als Referenz genutzte libfreefare, die für einen
2K3DES-Schlüssel `AUTHENTICATE_LEGACY`/`0x0A` wählt — nachvollziehbar,
libfreefare stammt primär aus der EV0/EV1-Ära) sendete bisher immer
`0x0A`. Die Karte akzeptiert `0x0A` zwar SYNTAKTISCH (Status `0x00` in
Schritt 2), verarbeitet es aber intern offenbar anders als `0x1A` — exakt
das erklärt das beobachtete Muster ("formal angenommen, aber die eigene
Rückprüfung passt nie"). Krypto-Struktur und IV-Verkettung sind zwischen
`0x0A` und `0x1A` laut Proxmark3-Quellcode IDENTISCH (beide nutzen
verkettetes CBC über die gesamte Sitzung, normale Verschlüsselung für den
PCD→PICC-Schritt) — nur das Kommandobyte unterscheidet sich. **Fix:**
`desfireAuthDes3()` sendet jetzt `0x1A` statt `0x0A`. Die zuvor (Nachtrag
1) eingebaute "Legacy sendet mit Entschlüsselungs- statt
Verschlüsselungs-Chiffre"-Sonderbehandlung (basierend auf libfreefares
`AUTHENTICATE_LEGACY`-Fallunterscheidung) wurde wieder entfernt — sie war
beim Werks-Nullschlüssel ohnehin mathematisch wirkungslos (siehe
Nachtrag 1) und passt nicht zum jetzt bestätigten EV1-Pfad.

**Zwei weitere, ebenfalls gegen den Proxmark3-Quellcode verifizierte
Bugfixes** (beide erst NACH erfolgreicher Authentifizierung relevant,
für `desfireChangeKeySame()` direkt im Anschluss — ohne sie wäre
"MASTER-PW SETZEN" trotz jetzt erfolgreicher Authentifizierung
vermutlich am ChangeKey-Schritt gescheitert):

1. **IV nach Authentifizierung ist 0, nicht der letzte Chiffretext-
   Block.** `desfireAuthDes3()`/`desfireAuthAes()` gaben bisher über
   `ivOut` den letzten ausgetauschten Chiffretext-Block zurück — laut
   Proxmark3 (`memset(dctx->IV, 0, ...)` direkt nach der
   Sitzungsschlüssel-Berechnung) ist der korrekte Start-IV für ein
   direkt folgendes Kommando wie ChangeKey aber schlicht **Null**.
2. **Sitzungsschlüssel-Sonderfall bei K1==K2 (Werks-Default!).** Die
   normale Formel `RndA[0:4] + RndB[0:4] + RndA[4:8] + RndB[4:8]` gilt
   nur, wenn die beiden 8-Byte-Hälften des Authentifizierungsschlüssels
   verschieden sind. Beim Werks-Default (16 Nullbytes, K1=K2, siehe
   Kopfkommentar bei `desfireDes3SetKey()`) muss die zweite Hälfte des
   Sitzungsschlüssels stattdessen die ERSTE wiederholen (Proxmark3-
   Kommentar: *"If the 3Des key first 8 bytes = 2nd 8 Bytes then we are
   really using Singe Des... we need to set the session key such that
   the 2nd 8 bytes = 1st 8 bytes"*) — genau der Fall, der bei
   "MASTER-PW SETZEN" mit noch unverändertem Werksschlüssel eintritt.

**Wichtiger Hinweis:** Dieser Fix wurde bisher NICHT an echter Hardware
gegengetestet (nur die Proxmark3-Gegenprobe bestätigt den Schlüssel und
das Kommando-Byte, nicht unseren eigenen Code erneut). Bitte nach dem
nächsten Flash erneut "MASTER-PW SETZEN" probieren und das Log schicken.

**Nachtrag 5 — 0x1A behebt es NICHT; Krypto-Algorithmus jetzt aber
BEWIESEN korrekt, Verdacht auf Empfangsseite.** Test mit dem 0x1A-Fix an
echter Hardware: gleiches Muster wie zuvor (Status `0x00`, eigene
Rückprüfung schlägt bei allen 3 unabhängigen Versuchen fehl). Um das ein
für alle Mal zu klären, wurde der komplette Proxmark3-Algorithmus
(`DesfireAuthenticateEV1()`) 1:1 in Python nachgebaut (nicht nur
einzelne Formeln verglichen, sondern die komplette Funktion inkl.
`DesfireCryptoEncDecEx`/`DesfireCryptoEncDecSingleBlock`) und mit den
echten geloggten Werten eines Versuchs gefüttert:

- Das von unserem eigenen Code gesendete `EncAB` (16 Byte) ist
  **bit-identisch** mit dem, was der nachgebaute Proxmark3-Algorithmus
  aus denselben `RndA`/`RndB`-Werten berechnet. Das beweist: unsere
  Sende-Seite (Verschlüsselung, IV-Verkettung, K1/K2-Handhabung) ist zu
  100 % korrekt, keine Vermutung mehr.
- ABER: derselbe nachgebaute Proxmark3-Algorithmus scheitert AUCH beim
  Verifizieren der ECHTEN Kartenantwort (`EncRndAResp`) aus unserem Log --
  mit dem exakt gleichen falschen Ergebnis wie unser eigener Code.

Das verschiebt den Verdacht eindeutig: **nicht** unser Kryptografie-
Algorithmus (jetzt bewiesen korrekt), sondern entweder (a) die vom PN532
tatsächlich empfangenen 8 Byte `EncRndAResp` stimmen nicht mit dem
überein, was die Karte wirklich gesendet hat (Empfangsseite/Framing/
Pufferproblem beim Adafruit_PN532-`inDataExchange()` speziell für DIESE
Antwort), oder (b) die tatsächliche Antwortlänge weicht von den
angenommenen 9 Byte (1 Status + 8 Nutzdaten) ab und wir lesen die
falschen Bytes. **Ergänzt:** `desfireAuthDes3()` loggt jetzt zusätzlich
die exakte `respLen` UND den kompletten Rohantwortpuffer (nicht nur die
interpretierten 8 Byte) direkt nach Schritt 2 -- das nächste Log zeigt
damit, ob hier tatsächlich mehr/andere Bytes ankommen als erwartet.

Nebenbei gefunden und behoben: Die Produktionswoche/-jahr-Anzeige im
DESFire-Tiefenauslese-Log zeigte "Woche 39 / 2037" statt korrekt
"Woche 27 / 2025" (Vergleich mit TagInfo) -- `productionWeek`/
`productionYear` sind BCD-kodiert (NXP-Konvention), wurden aber als
normale Binärzahl ausgegeben (Byte `0x27` dezimal ausgegeben ergibt
fälschlich "39" statt BCD-dekodiert korrekt "27"). Jetzt mit echter
BCD-Dekodierung.

**Nachtrag 6 — Authentifizierung funktioniert jetzt tatsächlich!** Test
mit dem 0x1A-Fix (Nachtrag 4) an echter Hardware: **kein**
"RndA-Rückprüfung fehlgeschlagen" mehr im Log — die Authentifizierung
gelingt jetzt beim ersten Versuch. Der Ablauf kam bis zu
`desfireChangeKeySame()`, die dort mit `INTEGRITY_ERROR` (Status `0x1E`)
scheiterte — ein neuer, aber klar eingegrenzter Fehler (die Karte
entschlüsselt das ChangeKey-Kryptogramm erfolgreich, verwirft es aber
wegen einer nicht passenden CRC-Prüfsumme).

Grund (wieder gegen den echten Proxmark3-Quellcode geprüft,
`DesfireChangeKey()` in `desfirecore.c`): Die 2K3DES-Variante von
`desfireChangeKeySame()` verwendete noch die alte D40-Konvention (CRC16
NUR über den neuen Schlüssel). Da die Sitzung inzwischen über den
EV1-Kanal läuft (Kommando `0x1A`, siehe Nachtrag 4), erwartet die Karte
aber die EV1-Konvention: **CRC32 über Kommandobyte + KeyNo + NewKey**
(Proxmark3-Kommentar: *"EV1 Checksum must cover: \<KeyNo\>
\<PrevKey XOR Newkey\>"*). Die AES-Variante war davon nicht betroffen —
sie deckte den Kommando-Header schon immer mit ab. **Fix:** 2K3DES-Zweig
auf CRC32 über `(0xC4, KeyNo, NewKey)` umgestellt, exakt wie im
AES-Zweig, nur ohne KeyVersion-Byte.

**Wichtiger Hinweis:** Auch dieser Fix ist bisher NICHT an echter
Hardware gegengetestet. Bitte neu flashen, "MASTER-PW SETZEN" erneut
versuchen und das Log schicken — falls es weiterhin an ChangeKey
scheitert, sind die wahrscheinlichsten verbliebenen Verdächtigen die
Sitzungsschlüssel-Formel oder der Start-IV (beide erst in Nachtrag 4
eingeführt und noch nie an echter Hardware bestätigt, da vorher die
Authentifizierung selbst schon scheiterte).

**Nachtrag 7 — BESTÄTIGT: kompletter Schreib-Workflow funktioniert an
echter Hardware.** Test an echter Hardware: `MASTER-PW SETZEN` → App
erstellen → Guthaben einbuchen (`Credit`) → Guthaben abfragen
(`GetValue`) → Guthaben ausbuchen (`Debit`) → App löschen
(`DeleteApplication`) → `AUF STANDARD ZURÜCKSETZEN` — **alle 7 DESFire-
Schreibfunktionen laufen jetzt Ende-zu-Ende erfolgreich durch.** Damit
ist die Kernfrage dieser wochenlangen Fehlersuche (Abschnitte 12,
Nachträge 1-7) abgeschlossen: 2K3DES-Authentifizierung + alle
Schreibkommandos funktionieren an einer echten DESFire-EV3-Karte.

**Neuer, noch offener Fund dabei: `GetFileIDs` meldet mehr Dateien als
je angelegt wurden.** Die dedizierte `/desfire_log.txt` zeigte für die
gerade frisch erstellte App (genau 1 Value-Datei angelegt) stattdessen:

```
Anwendungen (AIDs): 1
-- AID 123456 --
   Authentifiziert mit Default-Schluessel (2K3DES).
   Dateien: 9
   Datei 0: Typ=Value, Kommunikation=Plain          <- echt, unsere Datei
   Datei 218: GetFileSettings fehlgeschlagen.        <- kann nicht echt sein
   Datei 113: GetFileSettings fehlgeschlagen.        <- (es gibt im Code
   Datei 114: GetFileSettings fehlgeschlagen.           keinen Pfad, der
   Datei 48: GetFileSettings fehlgeschlagen.             mehr als 1 Datei
   Datei 92: GetFileSettings fehlgeschlagen.             pro App anlegt)
   Datei 129: GetFileSettings fehlgeschlagen.
   Datei 38: GetFileSettings fehlgeschlagen.
   Datei 34: GetFileSettings fehlgeschlagen.
```

Datei 0 ist echt (unsere Value-Datei), die restlichen 8 Dateinummern
sehen nach Zufallsbytes aus. Da `desfireGetFileIDs()` `len` direkt aus
der PN532-Antwortlänge übernimmt (kein eigener Parsing-Bug gefunden --
Code gegengeprüft), muss die zugrunde liegende `inDataExchange()`-Antwort
selbst 10 statt der erwarteten 2 Bytes (Status + 1 Dateinummer) enthalten
haben. Das erinnert an die vom Tutorial in Abschnitt 7 unabhängig
beschriebene PN532-Eigenheit (interner Speicher/Puffer liefert unter
bestimmten Bedingungen scheinbar zufälligen Inhalt statt eines
sauberen Fehlers) -- hier allerdings bei einer winzigen Anfrage, nicht
bei einer großen Übertragung wie dort, und aufgetreten nach einer
Serie vieler schneller Authentifizierungen/Kommandos hintereinander
(möglicherweise state-bezogen: PN532- oder Karten-Sitzung nicht sauber
zwischen den vielen Vorgängen zurückgesetzt). **Noch nicht behoben, nur
diagnostiziert:** `desfireGetFileIDs()` loggt jetzt bei `len > 1`
(für dieses Projekt aktuell immer verdächtig, da wir nie mehr als 1
Datei pro App anlegen) einen Hex-Dump der rohen Antwortbytes -- damit
gibt es beim nächsten Auftreten echte Rohdaten statt nur die
interpretierten (falschen) Dateinummern. Siehe Abschnitt 15 (Stage 6).

## 13. Mehrere WLAN-Netzwerke (Stage 5, `WiFiMulti`)

`wifi_secrets.h` (Stage 5) unterstützt jetzt beliebig viele
SSID/Passwort-Paare statt nur einem festen Netzwerk — praktisch z. B. für
Zuhause + Werkstatt + Handy-Hotspot in derselben Datei:

```cpp
static const WifiSecretEntry WIFI_SECRETS[] = {
  { "netzwerk-1", "passwort-1" },
  { "netzwerk-2", "passwort-2" },
  // beliebig mehr ...
};
```

Umgesetzt über die ESP32-Arduino-Core-Klasse `WiFiMulti`: `setup()`
registriert alle Einträge einmal per `wifiMulti.addAP(...)`; der
WLAN/ESP-NOW-Umschalter (EINSTELLUNGEN-Untermenü) ruft beim Einschalten
`wifiMulti.run()` statt des bisherigen fest verdrahteten `WiFi.begin(SSID,
PASSWORT)` auf — verbindet automatisch mit dem stärksten gerade in
Reichweite befindlichen Netzwerk aus der Liste. Bricht die Verbindung ab
(Netzwerk kurz außer Reichweite o. ä.), versucht ein periodisches
Reconnect-Polling in `loop()` alle 15s erneut (`WIFI_MULTI_RETRY_INTERVAL_MS`)
— deutlich seltener als das reine 1s-Status-Polling für die WLAN-IP-Zeile,
weil `wifiMulti.run()` bei fehlender Verbindung selbst einen blockierenden
`WiFi.scanNetworks()`-Aufruf durchführt und das bei jedem 1s-Tick ESP-NOW/
Touch-Reaktionszeit spürbar stören würde. Alte `wifi_secrets.h`-Dateien im
Ein-Netzwerk-Format (`#define WIFI_SSID`/`WIFI_PASSWORD`) funktionieren
**nicht** mehr automatisch — Datei nach dem neuen `WifiSecretEntry`-Array-
Format aus `wifi_secrets.example.h` aktualisieren.

## 14. Ausblick — bewusst NICHT Teil dieses Repositories

Dieses Repository hat einen klar begrenzten Zweck: **alle Board- und
PN532/DESFire-Eigenheiten finden und lösen**, mit Testfirmware pro
Stage — NICHT die eigentliche Terminal-Anwendung entwickeln. Folgendes
gehört bewusst zu einem SPÄTEREN, eigenen Projekt (der echten
Terminal-Firmware), nicht hierher:

- Das kombinierte Gesamtprojekt/Terminal, das alle Subsysteme
  gleichzeitig und produktiv nutzt.
- CMAC-Schutz/Enciphered-Kommunikation nach der Authentifizierung,
  EV2-Secure-Messaging, 3K3DES, ChangeKey für einen anderen als den
  authentifizierten Schlüssel.
- Pro-Karte diversifizierte Schlüssel (Schlüsselableitung aus UID +
  Master-Key, z. B. nach NXP AN10922) statt eines einzigen, im
  Quellcode fest hinterlegten Schlüssels.
- Buchungshistorie (Linear-/Cyclic-Record-Dateien) und alles, was damit
  zusammenhängt (siehe Abschnitt 15).
- Jegliche Geschäftslogik (Preise, Berechtigungen, Benutzerverwaltung).

## 15. Stage 6 — Robustheit, Randfälle und verbleibende Board-/Modul-Eigenheiten

Nach dem Kernerfolg (Abschnitt 12, Nachtrag 7: alle 7 DESFire-
Schreibfunktionen laufen Ende-zu-Ende) ist die nächste Stage **kein
neues Feature**, sondern das systematische Durchtesten und Absichern
dessen, was schon da ist — passend zum eigentlichen Zweck dieses
Repositories. Geplanter Umfang, geordnet nach Priorität:

**A) Noch offene Bugs (zuerst):**
1. ~~`GetFileIDs`-Anomalie~~ — Ursache gefunden, Gegenmaßnahme eingebaut,
   siehe "Ergebnisse der ersten Testrunde" unten. Bitte beim nächsten
   Auftreten trotzdem nochmal die neuen Log-Zeilen schicken, um die
   Gegenmaßnahme selbst zu bestätigen.

**B) Bisher ungetestete Codepfade:**
2. AES-128-Authentifizierung (`desfireAuthAes()`) — nie erfolgreich
   gegen echte Hardware getestet, da die Testkarte einen 2K3DES-
   Schlüssel in Slot 0 hat. Testweise eine App mit `aesKeys=true`
   anlegen (der Parameter existiert in `desfireCreateApplication()`,
   wird aber vom UI aktuell nicht genutzt) und denselben ChangeKey-/
   Credit-/Debit-Ablauf gegen einen AES-Schlüssel durchspielen.
3. AES-ChangeKey-Zweig (CRC32 mit KeyVersion) — aus demselben Grund nie
   getestet.

**C) Randfälle/Grenztests (kein neuer Code nötig, nur gezieltes Testen):**
4. `Debit` unter die Untergrenze (`BOUNDARY_ERROR` erwartet).
5. `GetValue` direkt nach `Credit`, aber vor `CommitTransaction` --
   zeigt das den alten oder den neuen Wert?
6. Karte mitten in einem mehrstufigen Vorgang (Auth, ChangeKey,
   CreateApplication, Credit vor Commit) wegziehen -- sauberer
   Fehlschlag ohne Hänger/Absturz?
7. Viele Zyklen hintereinander (Master-Key setzen/zurücksetzen, App
   erstellen/löschen) -- Stabilität über Zeit, kein Speicherleck, kein
   ESP32-Reset.
8. ESP-NOW-Traffic von einem zweiten Board UND DESFire-Vorgänge
   gleichzeitig -- Timing-Interferenzen?

**D) Hardware-Varianz (abhängig von Beschaffung):**
9. Andere Kartengrößen (4K/8K/16K statt der bisher getesteten 2K) --
   siehe Antwort weiter oben im Gespräch, sollte laut Code-Analyse ohne
   Änderung funktionieren, aber nie an echter größerer Karte bestätigt.
10. Eine echte EV1- oder EV2-Karte, falls verfügbar -- unsere
    Kompatibilitäts-Annahme (Abschnitt 12, Antwort zu "EV1 vs. EV2 vs.
    EV3") ist bisher nur protokoll-logisch begründet, nicht unabhängig
    bestätigt.
11. Ein zweites PN532-Modul (andere Charge/anderer Anbieter) -- das im
    Tutorial (Abschnitt 7) beschriebene Problem mit zu schwachen
    Nachbau-Modulen (RF-Feld bricht bei Auth ein) an unserer eigenen
    Hardware ausschließen oder bestätigen.

### Ergebnisse der ersten Testrunde

**Test 6 (Karte mitten im Vorgang wegziehen) durchgeführt — zwei echte
Funde dabei:**

1. **"App wurde trotz angezeigtem Fehlschlag angelegt" -- erklärt, kein
   Datenverlust-Bug.** Log zeigte:
   ```
   desfireCreateApplication(): UNBEKANNT (status=0xDE)
   desfireOpCreateApp(): CreateApplication fehlgeschlagen (existiert sie schon?).
   ```
   `0xDE` ist der DESFire-Code für `DUPLICATE_ERROR` ("Anwendung
   existiert bereits") -- unsere `desfireStatusName()` kannte den Code
   noch nicht und zeigte "UNBEKANNT". Der tatsächliche Ablauf: beim
   ERSTEN (abgebrochenen) Versuch hatte die Karte `CreateApplication`
   bereits verarbeitet, BEVOR das Wegziehen die Rückmeldung zum Reader
   verhinderte -- der Reader sah nur einen Kommunikationsfehler und
   zeigte "fehlgeschlagen", obwohl die Karte die Aktion schon
   ausgeführt hatte. Beim NÄCHSTEN Versuch meldet die Karte dann
   korrekterweise `0xDE` ("gibt's schon"), was ebenfalls als
   "fehlgeschlagen" angezeigt wurde. Das ist **kein Bug in der
   Zuverlässigkeit** -- es ist erwartetes, physikalisch bedingtes
   NFC-Verhalten: wird das RF-Feld genau zwischen "Karte hat das
   Kommando verarbeitet" und "Antwort beim Reader angekommen"
   unterbrochen, weiß der Reader nicht mehr sicher, ob die Aktion
   durchging. **Wichtige Konsequenz fürs spätere Terminal:** nach einem
   gemeldeten Fehlschlag NIE einfach nochmal automatisch versuchen, ohne
   vorher den tatsächlichen Kartenzustand zu prüfen (genau das haben wir
   hier über die Tiefenauslesung zufällig getan und dadurch bemerkt).
   **Fix:** `0xDE` (`DUPLICATE_ERROR`), `0xCA` (`COMMAND_ABORTED`) und
   `0xEE` (`MEMORY_ERROR`) zu `desfireStatusName()` ergänzt, damit
   solche Fälle beim nächsten Mal klar im Log stehen statt "UNBEKANNT".

2. **`GetFileIDs`-Anomalie: Ursache eingegrenzt, Gegenmaßnahme
   eingebaut.** Drei unabhängige Vorkommen im selben Testlauf zeigten
   IMMER exakt 8 zusätzliche, unplausible "Dateien" (z. B. 233, 186, 21,
   219, 22, 250, 213, 239) -- zu regelmäßig für reines Zufallsrauschen,
   und 8 Byte entspricht genau der Blockgröße unserer DES3-
   Authentifizierungsantworten. Der tatsächliche Adafruit_PN532-
   Quellcode (`inDataExchange()`, direkt von GitHub geladen und
   gelesen) zeigt: die Antwortlänge wird nur übernommen, wenn die vom
   **PN532-Chip selbst** mitgesendete Längen-Prüfsumme (LCS) passt --
   das schließt einen Treiber-/Auswertungsbug in unserem Code oder der
   Adafruit-Bibliothek aus. Wenn die Länge dennoch falsch ist, muss der
   PN532-CHIP selbst eine falsche (aber intern konsistente) Länge
   melden -- vermutlich Restdaten im chip-internen Zielantwortspeicher
   von der unmittelbar vorangegangenen 8-Byte-Authentifizierungsantwort.
   Das ist eine Hardware-/Chip-Eigenheit, kein Software-Bug, und lässt
   sich nicht in der Treiber-Bibliothek reparieren.
   **Gegenmaßnahme:** `desfireGetFileIDs()` wiederholt die Abfrage bis
   zu 2x, sobald mehr als 1 Datei gemeldet wird, und übernimmt nur ein
   Ergebnis, das sich in zwei aufeinanderfolgenden Abfragen exakt
   bestätigt (ein echter Speichermüll-Fehlschlag sollte sich nicht
   identisch wiederholen). Funktioniert unverändert auch für echte
   Apps mit mehreren Dateien (z. B. spätere Record-Dateien), solange die
   Wiederholung dasselbe Ergebnis liefert.

Punkte A-C brauchen keine neue Hardware und keinen neuen Code (außer
punktuellen Diagnose-Ergänzungen wie in Nachtrag 7) -- das ist der
sinnvolle nächste Schritt. D hängt davon ab, welche Zusatz-Hardware
verfügbar ist.
