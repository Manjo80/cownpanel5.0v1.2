# Firmware — Stufenweiser Funktionstest CrowPanel Advance 5.0

Fünf unabhängige, einzeln flashbare Arduino-Sketches, die die Hardware
dieses Boards Schritt für Schritt aufbauen und testen — in der Reihenfolge,
in der sie üblicherweise ans Laufen gebracht werden:

| Ordner | Testet | Status |
|---|---|---|
| `01_display_touch_buzzer/` | RGB-Display, GT911-Touch, Backlight/Buzzer (STC8H1K28) | fertig |
| `02_wifi_espnow/` | WLAN-Modus + ESP-NOW Senden/Empfangen | fertig |
| `03_rtc/` | PCF8563-Echtzeituhr, optional NTP-Sync | fertig |
| `04_sd_card/` | SD-/TF-Karte über SPI | fertig |
| `05_pn532_spi/` | PN532-NFC-Modul über Software-SPI (Basis: Firmware-Version + UID-Read) | fertig |

Jeder Ordner ist ein vollständiger, eigenständiger Arduino-Sketch (Ordnername
= `.ino`-Dateiname, wie von der Arduino-IDE verlangt) und enthält seine
eigene Kopie von `pins.h` und den gemeinsam genutzten Hilfs-Headern. Die
kanonische Quelle für alle Pin-/Adresszuordnungen ist
[`common/pins.h`](common/pins.h) — bei Änderungen dort zuerst anpassen und
dann in jeden `0X_*`-Ordner kopieren.

**Empfohlenes Vorgehen:** jede Stufe einzeln flashen und auf echter Hardware
verifizieren, bevor die nächste angegangen wird. Jeder Sketch ist bewusst so
gebaut, dass er für sich allein lauffähig ist — es gibt (noch) kein
kombiniertes Gesamtprojekt, das alle fünf Subsysteme gleichzeitig nutzt.

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

Der GT911 wählt seine I2C-Adresse (0x5D oder 0x14) beim Power-On anhand des
Pegels an seinem INT-Pin. Da dieser Pin auf diesem Board nicht angesteuert
wird, floatet der Pegel und die Adresse kann zwischen Boots wechseln — das
äußert sich als "Touch geht mal, mal nicht" bzw. im schlimmsten Fall als
Aussetzer beim Display-Bringup. Jeder Stage-Sketch sondiert deshalb beide
Adressen zur Laufzeit (`probeGT911Address()` in `touch_probe.h`), **bevor**
`lcd.init()` aufgerufen wird, statt eine Adresse fest zu verdrahten.

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
kollidiert die serielle Konsole mit dem PN532.

SD (Stage 4, IO4/5/6) und PN532 (Stage 5, IO43/44/2/8) nutzen unterschiedliche
Pins und können bei S1/S0=1/1 gleichzeitig betrieben werden — in einem
künftigen kombinierten Sketch ist das also kein Konflikt.

## 7. Bekannte Stolpersteine

- **Display bleibt schwarz:** PSRAM nicht auf OPI PSRAM gestellt.
- **Touch reagiert nicht / Display manchmal schwarz, manchmal nicht:**
  GT911-Adresswechsel, siehe Abschnitt 4 — durch `probeGT911Address()`
  bereits behoben; falls trotzdem instabil, echten I2C-Pullup/Verkabelung
  prüfen.
- **ESP-NOW-Callback kompiliert nicht:** Callback-Signaturen sind
  Core-Versions-abhängig. Dieser Code ist auf Core 3.x geschrieben
  (`esp_now_recv_info_t*`); auf Core 2.x muss der Recv-Callback stattdessen
  `void(const uint8_t *mac_addr, const uint8_t *data, int len)` lauten.
- **PN532 antwortet nicht / GetFirmwareVersion schlägt fehl:** DIP-Schalter
  auf dem Modul falsch (muss SPI = Sw1 OFF/Sw2 ON sein), oder
  `PN532_PACKBUFFSIZ`-Patch fehlt.
- **DESFire-Kommandos schlagen nach erfolgreichem UID-Read fehl:**
  `readPassiveTargetID()` setzt nicht die interne `_inListedTag`-Variable —
  für `inDataExchange()`-Kommandos muss stattdessen `inListPassiveTarget()`
  zur Kartenaktivierung verwendet werden (in Stage 5 absichtlich noch nicht
  gebraucht, da dort nur die Basisverbindung getestet wird).

## 8. Ausblick — nicht Teil dieser fünf Stages

- Kombiniertes Gesamtprojekt, das alle Subsysteme gleichzeitig nutzt.
- DESFire-AES-Authentifizierung und Applikations-Lebenszyklus über PN532
  (AuthenticateAES, CreateApplication, Credit/Debit, ChangeKey, CMAC-Schutz
  nach der Auth) — deutlich größerer Umfang als der Basis-SPI-Test hier.
