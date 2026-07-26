# CrowPanel Advance 5.0 — DESFire-Terminal — Projektdokumentation

**Board:** Elecrow CrowPanel Advance 5.0, SKU DIS02050A, ESP32-S3-WROOM-1 N16R8
**Stand:** Funktionstest DESFire-Lebenszyklus über PN532/SPI abgeschlossen; RGB-Display zeigt aktuell intermittierendes Verhalten (siehe Abschnitt 9)

---

## 1. Getestete Toolchain (funktionierende Basis)

| Komponente | Version |
|---|---|
| ESP32 Arduino Core | 3.3.11 |
| LovyanGFX | 1.2.26 (Library Manager) |
| PN532-Library (I2C-Versuch, verworfen für DESFire) | Elechouse PN532 + PN532_I2C |
| PN532-Library (SPI, funktioniert) | Adafruit_PN532 1.3.4, **modifiziert**: `PN532_PACKBUFFSIZ` 64→255, `inDataExchange`-Timeout 1000→5000 |
| RTC | Direkter Wire-Zugriff, keine Library (PCF8563) |

**Wichtig:** Auf Core 3.3.11 kompiliert die ältere Elecrow-LovyanGFX (1.1.16) nicht — Core-interne Symbole (`lcd_periph_signals`, `gpio_hal_iomux_func_sel`, `i2c_signal_conn_t::module`) wurden umbenannt. Nur LovyanGFX 1.2.26 verwenden. Nur **eine** LovyanGFX-Version darf im `libraries`-Ordner liegen.

---

## 2. Board-Einstellungen (Arduino IDE, Werkzeuge-Menü)

| Einstellung | Wert |
|---|---|
| Board | ESP32S3 Dev Module |
| PSRAM | **OPI PSRAM** (kritisch — ohne bleibt Display schwarz, Framebuffer 800×480×2 Byte = 768 KB braucht PSRAM) |
| Flash Size | 16 MB (128 Mb) |
| Partition Scheme | Huge APP (3 MB No OTA / 1 MB SPIFFS) |
| Flash Mode | QIO 80 MHz |
| CPU Frequency | 240 MHz (WiFi) |
| USB CDC On Boot | Disabled (Standardfall) / Enabled nur wenn UART0-Pins 43/44 anderweitig gebraucht werden |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |

PSRAM und Partition Scheme springen bei Core-Wechseln häufig auf Default zurück — nach jedem Core-Wechsel prüfen.

---

## 3. I2C-Bus (GPIO 15/16)

```
Wire.begin(15, 16, 100000);   // SDA=15, SCL=16, 100 kHz
```

Header-Aufdruck: `IO16_SCL / IO15_SDA / 3V3 / GND`

| Adresse | Gerät |
|---|---|
| 0x5D (oder 0x14, siehe Abschnitt 9) | GT911 Touch-Controller |
| 0x51 | PCF8563 Echtzeituhr |
| 0x30 | STC8H1K28 — Backlight- und Buzzer-Steuerung |
| 0x24 | Elechouse PN532 (nur im I2C-Modus, für DESFire nicht verwendet) |

---

## 4. RGB-Display

**Treiber-IC:** ST7262 (reiner RGB-Parallel-Controller)

**Timing:**
```cpp
cfg.freq_write = 16000000;   // 16 MHz — von 21 MHz gesenkt, behebt seitliches Flackern
```
Porches: hsync/vsync front=8, pulse=4, back=8 (Elecrow-Standardwerte, funktionieren bei 16 MHz).

**Datenpins:**
| Farbe | GPIO |
|---|---|
| Blau B0–B4 | 21, 47, 48, 45, 38 |
| Grün G0–G5 | 9, 10, 11, 12, 13, 14 |
| Rot R0–R4 | 7, 17, 18, 3, 46 |
| Sync | DE=42, VSYNC=41, HSYNC=40, PCLK=39 |

Framebuffer liegt im PSRAM (`cfg.use_psram = 1`).

---

## 5. Touch (GT911)

Läuft über LovyanGFX `Touch_GT911`, gleicher I2C-Bus wie RTC/Backlight (15/16).

```cpp
cfg.i2c_port = I2C_NUM_0;
cfg.pin_sda = 15; cfg.pin_scl = 16;
cfg.pin_rst = -1; cfg.pin_int = -1;   // Reset/Interrupt nicht angesteuert
cfg.freq = 400000;
cfg.i2c_addr = 0x5D;   // ODER 0x14 — siehe Abschnitt 9
```

**Bekannter GT911-Vorbehalt:** Da `pin_int = -1` (Interrupt-Pin nicht vom ESP32 angesteuert), wählt der GT911-Chip seine I2C-Adresse (0x5D oder 0x14) anhand des Pegels an diesem Pin **beim Power-On**. Da der Pegel floatet, kann die Adresse zwischen Boots wechseln — siehe offenes Problem in Abschnitt 9.

---

## 6. RTC (PCF8563)

- Adresse 0x51, Zeitregister **ab 0x02** (nicht 0x04 wie beim ähnlich benannten PCF85063).
- Direkter Wire-Zugriff mit BCD-Umrechnung, keine externe Library nötig.
- Pufferung über CR1220-Knopfzelle.
- VL-Bit (Bit 7 im Sekundenregister) zeigt an, ob die Zeit nach Spannungsausfall unzuverlässig ist.

---

## 7. WiFi + NTP-Zeitsync

```cpp
configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "europe.pool.ntp.org");
```
EU-Zeitzonenregel für Luxemburg inkl. automatischer Sommerzeitumstellung. Nach erfolgreichem NTP-Sync wird die Zeit in die RTC geschrieben (`rtcWrite`), damit sie auch ohne Netz weiterläuft.

---

## 8. Backlight & Buzzer (STC8H1K28, Adresse 0x30)

Ein einzelnes Byte an 0x30 senden:

| Wert | Funktion |
|---|---|
| 0 | Backlight maximale Helligkeit |
| 244 | Backlight minimale Helligkeit |
| 245 | Backlight aus |
| 246 | Buzzer an |
| 247 | Buzzer aus |

Reihenfolge wichtig: Backlight-Kommandos nach `lcd.init()` senden, nicht davor — sonst überschreibt die Panel-Initialisierung den Zustand.

---

## 9. OFFENES PROBLEM — intermittierendes Display-Verhalten

**Symptom:** Mit unverändertem, nachweislich funktionierendem Code (Uhr + Touch-Farbwechsel) zeigt das Display bei manchen Boots ein korrektes farbiges Bild mit reagierendem Touch, bei anderen Boots bleibt der Bildschirm schwarz (Backlight geht an, aber kein Bildinhalt), während Touch und Buzzer (beide I2C) weiterhin funktionieren oder auch nicht — das Verhalten ist nicht reproduzierbar bei identischem Code und identischer Hardware.

**Ausgeschlossene Ursachen (durch gezielte Tests widerlegt):**
- Doppelte/falsche LovyanGFX-Version — geprüft, nur eine Version (1.2.26) im `libraries`-Ordner vorhanden.
- Fehlende PSRAM-Konfiguration — geprüft, steht korrekt auf OPI PSRAM.
- Fehler in der modularen Code-Struktur (`display.h`/`touch.h`) — durch Rückgriff auf den exakten Original-Sketch (`rgbdisplay.h` + Einzeldatei-Test) ausgeschlossen; Verhalten trat auch dort auf.
- Bus-Konkurrenz mit PN532 — durch Tests komplett ohne PN532-Code (`touch_only_test`) ausgeschlossen.

**Aktuelle Haupthypothese:** GT911 wählt beim Power-On zufällig zwischen den Adressen 0x5D und 0x14 (da `pin_int = -1`, siehe Abschnitt 5). Der Code fragt fest nur eine Adresse ab. Wenn der Chip auf die nicht abgefragte Adresse aufwacht, initialisiert sich der Touch-Treiber nicht sauber, was je nach LovyanGFX-internem Ablauf auch die nachfolgende Panel-Bring-up-Sequenz stören kann.

**Laufende Diagnose-Schritte (Tests vorbereitet, Ergebnis ausstehend):**
1. `original_test` — 1:1 Referenzcode ohne jede Modifikation, zum Ausschluss von Refactoring-Fehlern.
2. `touch_only_test` — nur Display + Touch, kein PN532-Code im Build, zum Ausschluss von SPI/PN532-Interferenz.
3. `panel_only_test` — RGB-Panel **ganz ohne** `Touch_GT911`-Instanz im LGFX-Objekt, um zu prüfen, ob das Panel-Bild unabhängig vom Touch-Chip-Zustand zuverlässig kommt.
4. Adress-Tausch-Test (`rgbdisplay_addr14.h`) — `cfg.i2c_addr` von 0x5D auf 0x14, um zu prüfen, ob der Chip diesmal auf der Alternativadresse antwortet.

**Nächster Schritt, sobald geklärt:** Falls sich die Adress-Hypothese bestätigt, muss der Code beim Start beide Adressen (0x5D und 0x14) abfragen und `cfg.i2c_addr` dynamisch vor dem `LGFX`-Konstruktor setzen, statt sie fest zu verdrahten. Falls sich die Hypothese nicht bestätigt, ist eine tiefere Untersuchung bekannter LovyanGFX-RGB-Timing-Issues auf ESP32-S3/Core 3.3.11 nötig (z. B. GitHub-Issues zur `Bus_RGB`-Klasse).

---

## 10. PN532 NFC — zwei Anbindungswege getestet

### 10.1 I2C (funktioniert für UID-Lesen, NICHT für DESFire)

- Verkabelung: I2C-OUT-Header (SDA=15, SCL=16), DIP-Schalter am Modul auf I2C (Sw1=ON, Sw2=OFF).
- Mit der **Elechouse-Library** (`PN532`, `PN532_I2C`): UID-Lesen (`readPassiveTargetID`) funktioniert zuverlässig. `GetVersion`/DESFire-APDU-Austausch (`inDataExchange`) scheitert mit Transportfehler — die Library beherrscht das ISO14443-4-RATS-Handshake für DESFire nicht zuverlässig.
- Mit dem **modifizierten Adafruit_PN532** über I2C: UID-Lesen funktioniert, `GetVersion` schlägt zunächst mit "FEHLER" fehl — Ursache gefunden (siehe 10.3).

### 10.2 SPI (funktioniert für den vollständigen DESFire-Lebenszyklus)

**Problem:** Kein SPI-Header ist auf dem Board frei ohne Kompromisse:
- Wireless-Modul-Header führt IO2, IO8 (frei) sowie IO4/5/6, IO19/20 (durch Function-Select bzw. USB belegt).
- UART1-OUT (IO19/20) = USB-D+/D− beim ESP32-S3, bei USB-Betrieb nicht als GPIO nutzbar.
- UART0-OUT (IO43/44) frei nutzbar, wenn die serielle Konsole nicht über UART0 läuft (Debug-Ausgabe stattdessen aufs Display).

**Gewählte Lösung — zusammengesammelte freie Pins, kein Löten nötig, SD-Karte bleibt erhalten:**

```
PN532 VCC   -> 3V3   (Wireless-Header)
PN532 GND   -> GND   (Wireless-Header)
PN532 SCK   -> IO43  (UART0-OUT)
PN532 MOSI  -> IO44  (UART0-OUT)
PN532 MISO  -> IO2   (Wireless-Header)
PN532 SS    -> IO8   (Wireless-Header)
```

PN532-DIP-Schalter auf **SPI** (Sw1=OFF, Sw2=ON).

Software-SPI (Bitbang) über `Adafruit_PN532(SCK, MISO, MOSI, SS)`-Konstruktor, da die vier Pins aus zwei verschiedenen Headern zusammengesammelt sind und keinem festen Hardware-SPI-Block entsprechen.

**Alternative, nicht gewählt:** PN532 und SD-Karte auf gemeinsamem SPI-Bus (IO4/5/6, geteilt über CS), da SD-Pins nicht auf einem Header herausgeführt sind — würde Löten am SD-Slot oder an Testpunkten erfordern.

### 10.3 Der entscheidende Bugfix: `_inListedTag`

**Symptom:** UID wird korrekt gelesen (`readPassiveTargetID`), aber jedes nachfolgende DESFire-Kommando über `inDataExchange` schlägt fehl.

**Ursache:** `readPassiveTargetID()` setzt in der Adafruit_PN532-Library **nicht** die interne Variable `_inListedTag`, die `inDataExchange()` aber als Zieladresse für die APDU-Übertragung braucht.

**Fix:** Karte stattdessen mit `inListPassiveTarget()` aktivieren — diese Funktion setzt `_inListedTag` korrekt. Danach funktionieren alle DESFire-Kommandos über `inDataExchange`.

```cpp
// falsch für DESFire-Kommandos:
nfc.readPassiveTargetID(...);        // _inListedTag bleibt unbesetzt

// richtig:
nfc.inListPassiveTarget();           // setzt _inListedTag, danach inDataExchange nutzbar
```

---

## 11. Verifizierte Kartendaten (Testkarte)

Über `GetVersion` (0x60) und `FreeMemory` (0x6E) ausgelesen und unabhängig per NFC-TagInfo-App auf dem iPhone bestätigt:

| Eigenschaft | Wert |
|---|---|
| UID | `04 A4 AB 4A F6 21 90` (7 Byte, feste UID, keine Random-UID) |
| Hersteller | NXP Semiconductors |
| IC-Typ | DESFire |
| Generation | EV3 (HW-Major 0x33) |
| Speicher | 2048 Byte (2 KB) |
| Protokoll | ISO/IEC 14443-4 |
| Produktionsdatum | Woche 27, 2025 |

---

## 12. DESFire — AES-Authentifizierung und Lebenszyklus

### 12.1 Implementiert (`desfire.h`)

Vollständige **AES128-Challenge-Response-Authentifizierung** (Kommando `0xAA AuthenticateAES`) nach DESFire-Spezifikation, über ESP32-eigenes `mbedtls/aes.h` (kein selbstgebautes Krypto):

1. `AuthenticateAES(keyNo)` senden
2. `E(RndB)` von der Karte empfangen, mit Schlüssel entschlüsseln (IV=0)
3. `RndB'` = RndB um 1 Byte rotiert
4. `RndA` zufällig generieren (`esp_random()`)
5. `E(RndA || RndB')` verschlüsseln (IV = `E(RndB)`), an Karte senden
6. `E(RndA')` von der Karte empfangen, entschlüsseln, mit erwarteter Rotation von RndA vergleichen
7. Session-Key ableiten: `RndA[0..3] || RndB[0..3] || RndA[12..15] || RndB[12..15]`

Weitere implementierte Kommandos: `CreateApplication`, `SelectApplication`, `DeleteApplication`, `CreateValueFile`, `Credit`, `Debit`, `GetValue`, `ChangeKey` (nur für "aktuell authentifizierten Schlüssel selbst ändern").

**Wichtiger Sicherheitshinweis:** Nach der Authentifizierung laufen die Datei-Operationen (Credit/Debit/GetValue) aktuell im **Klartext-Kommunikationsmodus** (COMM.MODE Plain) — ausreichend für den Funktionstest, aber **nicht produktionssicher**. Für den Produktivbetrieb muss die Kommunikation nach der Auth zusätzlich CMAC-signiert bzw. verschlüsselt werden (nächste Ausbaustufe, noch nicht umgesetzt).

**Gefahr bei `changeKeyAES()`:** DESFire hat kein Backdoor-Reset. Wird ein Schlüssel geändert und der neue Wert verloren, ist die Karte für diesen Schlüssel-Slot dauerhaft gesperrt. Der Custom-Testschlüssel steht fest in `pins.h` (`DF_CUSTOM_KEY_BYTES`) — vor dem ersten produktiven Einsatz ändern und sicher verwahren.

### 12.2 Touch-Bedienoberfläche (7 Buttons)

| Button | Aktion |
|---|---|
| Master PW | Auth mit Werksdefault-Key (16× 0x00), `ChangeKey` auf Custom-Key |
| Standard | Auth mit Custom-Key, `ChangeKey` zurück auf Werksdefault |
| App erst. | `CreateApplication` (AID konfigurierbar), `CreateValueFile` mit Startwert 0 |
| Buchen | `Credit` (+500, Testwert), anschließend `CommitTransaction` |
| Nutzen | `Debit` (−200, Testwert), anschließend `CommitTransaction` |
| Abfragen | `GetValue` — aktueller Kontostand |
| App loesch | `DeleteApplication` |

Buzzer-Signal (`beep()`) wird bewusst **erst nach abgeschlossener Operation** ausgelöst, nicht beim Start — Signal für "Karte kann jetzt entfernt werden", nicht "Lesen beginnt".

---

## 13. Modulare Code-Struktur

Aufgeteilt in wiederverwendbare Header, um Wartbarkeit zu verbessern (Projekt-Ordner `terminal`):

| Datei | Inhalt |
|---|---|
| `pins.h` | Zentrale Wahrheit für alle GPIOs, I2C-Adressen, Backlight-Werte, DIP-Schalter-Dokumentation, DESFire-Testkonstanten |
| `display.h` | LovyanGFX-Konfiguration (erzeugt `lcd`-Objekt), Backlight/Buzzer-Funktionen, einfache Text-Konsole für Display-Debug-Ausgabe |
| `rtc.h` | PCF8563-Zugriff (lesen/schreiben/Zeitstring-Formatierung) |
| `wifitime.h` | WiFi-Verbindung + NTP-Sync, schreibt direkt in RTC |
| `nfc.h` | PN532-Basisfunktionen über SPI (`nfcBegin`, `nfcActivate`, `dfCommand`) |
| `desfire.h` | AES-Authentifizierung, Applikations-/Datei-Lebenszyklus |
| `touch.h` | Generisches Button-Raster für GT911-Touch |
| `terminal.ino` | Hauptsketch, bindet Header ein, verdrahtet Buttons mit Aktionen |

**Wichtiger Stolperstein:** Alte Einzeldatei-Testprojekte (`test.ino` + `rgbdisplay.h`) dürfen **nicht** im selben Ordner wie `terminal.ino` liegen — Arduino kompiliert alle `.ino`/`.h`-Dateien eines Ordners zusammen; zwei `setup()`-Funktionen oder zwei `LGFX`-Klassendefinitionen im selben Build führen zu unvorhersehbarem Verhalten.

---

## 14. DIP-Schalter-Referenz (Hardware, nicht per Code änderbar)

### Function-Select am CrowPanel (S1/S0)

| S1 | S0 | Modus | IO4 | IO5 | IO6 | zusätzlich |
|---|---|---|---|---|---|---|
| 0 | 0 | MIC & SPK | I2S SDIN | I2S BCLK | I2S LRCLK | MIC: IO19=CLK, IO20=DATA |
| 0 | 1 | WM (Wireless) | → Wireless-Header | | | |
| 1 | 1 | MIC & TF Card | SD MISO | SD SCK | SD MOSI | MIC: IO19=CLK, IO20=DATA, SD-CS=IO0 |

Für dieses Terminal (SD aktiv, PN532 nicht am Wireless-Header): **S1/S0 = 1/1**

### DIP am PN532-Modul (Sw1/Sw2)

| Sw1 | Sw2 | Modus |
|---|---|---|
| 0 | 0 | HSU (UART, Werk) |
| 1 | 0 | I2C |
| 0 | 1 | SPI |

Für diesen Aufbau: **Sw1/Sw2 = 0/1**

---

## 15. Offene Punkte / nächste Schritte

1. **Display-Intermittenz beheben** (Abschnitt 9) — höchste Priorität, blockiert zuverlässigen Dauerbetrieb.
2. **CMAC-/verschlüsselte Kommunikation nach DESFire-Auth** ergänzen — notwendig für Produktionssicherheit, aktuell nur Plain-Mode implementiert.
3. **DESFire-Kapazität 2K/4K generisch halten** — Code darf sich nicht auf feste Speichergrößen verlassen, sondern `GetFreeMemory` zur Laufzeit nutzen, damit derselbe Code auf beiden Kartengrößen läuft.
4. **Custom-Testschlüssel in `pins.h` vor Produktivbetrieb ändern** und sicher dokumentieren (Passwortmanager o. ä.), sonst Gefahr des dauerhaften Kartenverlusts.
5. **RFID-Card-Auswahl fürs Vereinsfest-Projekt klären:** feste UID (einfacher, klonbar) vs. Random-UID (sicherer, benötigt authentifizierte Sitzung um echte Karten-ID zu erhalten).
6. **Integration mit Django-Backend** — aktueller Stand ist reiner Gerätefunktionstest, Anbindung ans Backend (WebSocket/HTTP) noch nicht begonnen.
7. **ESP-NOW-Callback-Signatur** ist Core-Versions-abhängig (`wifi_tx_info_t` auf 3.x vs. `uint8_t*` auf 2.x) — bei künftigen Core-Wechseln beachten.
