// Stage 5 -- PN532 NFC ueber Software-SPI, Erweiterung von Stage 4
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// Baut auf Stage 4 auf: SD-Karten-Status, RTC-Anzeige, STELLEN/NTP-SYNC-
// Buttons und der komplette WLAN/ESP-NOW-Status (inkl. Log-Datei) bleiben
// erhalten. Neu dazu kommt der PN532-NFC-Leser: Firmware-Version beim Start
// + fortlaufendes UID-Lesen aufgelegter Karten. Der Bildschirm ist jetzt
// SEHR voll -- bewusst so gewaehlt (kompakter statt Funktionen wegzulassen).
//
// VERKABELUNG (siehe README.md Abschnitt 6 -- Pins aus zwei Headern
// zusammengesammelt, kein freier Hardware-SPI-Block, daher Software-SPI):
//   PN532 VCC  -> 3V3   (Wireless-Header)
//   PN532 GND  -> GND   (Wireless-Header)
//   PN532 SCK  -> IO43  (UART0-OUT)
//   PN532 MOSI -> IO44  (UART0-OUT)
//   PN532 MISO -> IO2   (Wireless-Header)
//   PN532 SS   -> IO8   (Wireless-Header)
// PN532-DIP-Schalter am Modul: SPI-Modus, Sw1=OFF, Sw2=ON.
//
// WICHTIG -- da IO43/44 = UART0 TX/RX sind, darf die serielle Konsole nicht
// ueber UART0 laufen, solange der PN532 dort haengt:
//   Arduino IDE: Werkzeuge -> USB CDC On Boot: Enabled
//   PlatformIO: build_flags ARDUINO_USB_MODE/ARDUINO_USB_CDC_ON_BOOT (siehe
//   platformio.ini in platformio/stage5_pn532_spi/)
//
// BENOETIGTE LIBRARY: Adafruit_PN532 (Arduino-IDE-Nutzer: Library Manager),
// MIT ZWEI PATCHES (siehe README.md Abschnitt 3), sonst schlagen laengere
// Antworten (z. B. GetFirmwareVersion bei manchen Modulklonen) fehl:
//   1. In Adafruit_PN532.h:  #define PN532_PACKBUFFSIZ 64   ->  255
//   2. In Adafruit_PN532.cpp, inDataExchange(): Timeout 1000 -> 5000 (ms)
//
// Dieser Test deckt NUR die Basis-Verbindung ab (Firmware-Version +
// UID-Read per readPassiveTargetID()). Fuer den vollen DESFire-Lebenszyklus
// waere stattdessen inListPassiveTarget() zur Kartenaktivierung noetig --
// siehe README.md, bewusst nicht Teil dieses Basis-Funktionstests.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <Adafruit_PN532.h>
#include <stdio.h>
#include <string.h>
#include "pins.h"
#include "rgb_panel.h"
#include "touch_standalone.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"
#include "rtc_pcf8563.h"
#include "desfire.h"

// Fuer echten NTP-Abgleich einkommentieren und wifi_secrets.h anlegen
// (Kopie von wifi_secrets.example.h in diesem Ordner, eigene Zugangsdaten
// eintragen). Ohne das geht STELLEN (Compile-Zeit) weiterhin, nur NTP-SYNC
// meldet dann "deaktiviert".
// #define USE_WIFI_NTP_SYNC
#ifdef USE_WIFI_NTP_SYNC
#include <time.h>
#include "wifi_secrets.h"
#endif

// canvas zeigt per setBuffer() direkt auf den von rgbPanelInit()
// bereitgestellten PSRAM-Framebuffer (esp_lcd_panel_rgb mit Bounce Buffer,
// siehe rgb_panel.h).
LGFX_Sprite canvas;

SPIClass sdSpi(HSPI);
const char *LOG_FILE_PATH = "/espnow_log.txt";

// Software-SPI-Konstruktor: (SCK, MISO, MOSI, SS) -- vier frei gewaehlte
// Pins, kein Hardware-SPI-Block, siehe README.md Abschnitt 6.
Adafruit_PN532 nfc(PIN_PN532_SCK, PIN_PN532_MISO, PIN_PN532_MOSI, PIN_PN532_SS);

struct Button {
  int x, y, w, h;
  const char *label;
};

// Kompakteres Layout als in Stage 4 (siehe update*Info()-Bereichsgrenzen
// unten) -- bewusst dichter gepackt statt Funktionen wegzulassen. Nochmal
// etwas enger als beim ersten Stage-5-Layout, um Platz fuer die zweite
// Zeile (DESFire-Zusammenfassung) im UID-Bereich zu schaffen.
Button btnSetCompile = { 40,  358, 340, 42, "STELLEN (Compile-Zeit)" };
Button btnNtpSync    = { 420, 358, 340, 42, "NTP-SYNC" };
Button btnBeep       = { 40,  408, 220, 46, "BEEP" };
Button btnBrightUp   = { 300, 408, 220, 46, "BACKLIGHT +" };
Button btnBrightDn   = { 560, 408, 220, 46, "BACKLIGHT -" };

uint8_t backlightPercent = 80;

const uint16_t bgColors[] = { TFT_NAVY, TFT_DARKGREEN, TFT_MAROON, TFT_BLACK };
uint8_t bgIndex = 0;

typedef struct __attribute__((packed)) {
  char     text[48];
  uint32_t counter;
} espnow_message_t;

espnow_message_t outgoing;
espnow_message_t lastReceived;
bool haveReceived = false;
uint8_t lastSrcMac[6] = {0};
bool lastSendOk = false;
bool haveSendResult = false;
uint32_t lastSendMs = 0;

// onDataRecv() laeuft im WiFi/ESP-NOW-System-Task, nicht im loop()-Task --
// siehe firmware/README.md Abschnitt 7. Nur Flag setzen, Zeichnen/Loggen
// passieren ausschliesslich in loop().
volatile bool recvPending = false;
const uint32_t SEND_INTERVAL_MS = 2000;
const uint32_t RTC_UPDATE_INTERVAL_MS = 1000;
const uint32_t SD_POLL_INTERVAL_MS = 2000;
const uint32_t PN532_POLL_INTERVAL_MS = 300;
const uint32_t PN532_DEBOUNCE_MS = 1000;

uint8_t broadcastAddr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// SD-Status: siehe checkSdCard() weiter unten -- erkennt Einstecken/
// Herausziehen waehrend des Betriebs.
bool sdMounted = false;
bool sdCheckedOnce = false;
char sdStatusMsg[64] = "SD-Karte: wird geprueft...";
uint16_t sdStatusColor = TFT_LIGHTGREY;

// PN532-Status: einmaliger Verbindungsversuch in setup() (siehe
// pn532Begin()) -- anders als bei der SD-Karte kein Live-Hotplug-Polling,
// da das fuer diesen Basistest nicht gefordert war.
bool pn532Ready = false;
char pn532StatusMsg[64] = "PN532: wird geprueft...";
uint16_t pn532StatusColor = TFT_LIGHTGREY;
char uidMsg[64] = "Noch keine Karte gelesen.";
// DESFire-Tiefenauslesung (siehe desfire.h + desfireDeepRead() unten) --
// Details landen in DESFIRE_LOG_PATH auf der SD-Karte, hier nur die kurze
// Ein-Zeilen-Zusammenfassung fuers Display.
char desfireSummaryMsg[80] = "";
const char *DESFIRE_LOG_PATH = "/desfire_log.txt";
uint32_t lastPn532AttemptMs = 0;
uint32_t lastUidReadMs = 0;

// Bereichsgrenzen der einzelnen Update-Funktionen -- an einer Stelle
// definiert, damit das fillRect() in der jeweiligen Funktion garantiert
// mit sich selbst konsistent bleibt. Kompakter als in Stage 4, um Platz
// fuer PN532-Status + UID-Zeile zu schaffen.
const int SD_INFO_Y = 112, SD_INFO_H = 26;
const int RTC_INFO_Y = 142, RTC_INFO_H = 34;
const int PN532_INFO_Y = 180, PN532_INFO_H = 26;
const int SEND_INFO_Y = 210, SEND_INFO_H = 42;
const int RECV_INFO_Y = 256, RECV_INFO_H = 40;
// UID_INFO jetzt zweizeilig: UID selbst + DESFire-Zusammenfassung darunter.
const int UID_INFO_Y = 300, UID_INFO_H = 50;

void drawButton(const Button &b, uint16_t fill) {
  canvas.fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
  canvas.drawRoundRect(b.x, b.y, b.w, b.h, 8, TFT_WHITE);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(2);
  canvas.setTextDatum(lgfx::middle_center);
  canvas.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
}

bool inside(const Button &b, int32_t x, int32_t y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

// Titel + eigene MAC-Adresse + alle fuenf Buttons -- aendert sich nur, wenn
// sich die Hintergrundfarbe aendert (Tap auf freie Flaeche).
void drawStaticParts() {
  canvas.fillScreen(bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(3);
  canvas.setCursor(20, 20);
  canvas.println("CrowPanel Advance 5.0");
  canvas.setTextSize(2);
  canvas.println("Stage 5: PN532 NFC + SD + RTC + WLAN/ESP-NOW");

  canvas.setCursor(20, 90);
  canvas.print("Eigene MAC: ");
  canvas.println(WiFi.macAddress());

  drawButton(btnSetCompile, TFT_DARKGREY);
  drawButton(btnNtpSync, TFT_DARKGREY);
  drawButton(btnBeep, TFT_DARKGREY);
  drawButton(btnBrightUp, TFT_DARKGREY);
  drawButton(btnBrightDn, TFT_DARKGREY);
}

// Nur die SD-Statuszeile -- wird ausschliesslich bei einem tatsaechlichen
// Zustandswechsel (Karte erkannt/entfernt) aus checkSdCard() aufgerufen.
void updateSdInfo() {
  canvas.fillRect(0, SD_INFO_Y, LCD_WIDTH, SD_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(sdStatusColor);
  canvas.setCursor(20, SD_INFO_Y + 3);
  canvas.print(sdStatusMsg);
}

// Nur die RTC-Zeile -- wird jede Sekunde aus loop() aufgerufen, unabhaengig
// von SD-/PN532-/ESP-NOW-Updates (eigener Bildschirmbereich).
void updateRtcInfo() {
  canvas.fillRect(0, RTC_INFO_Y, LCD_WIDTH, RTC_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(3);
  canvas.setCursor(20, RTC_INFO_Y);

  RtcDateTime dt;
  char buf[64];
  if (rtcRead(dt)) {
    rtcFormat(dt, buf, sizeof(buf));
    canvas.setTextColor(dt.voltageLow ? TFT_RED : TFT_GREENYELLOW);
    canvas.print(buf);
  } else {
    canvas.setTextColor(TFT_RED);
    canvas.print("RTC-Lesefehler (I2C pruefen)");
  }
}

// Nur die PN532-Statuszeile -- wird einmalig nach pn532Begin() gezeichnet
// (kein Live-Hotplug-Polling wie bei der SD-Karte, siehe oben).
void updatePn532Info() {
  canvas.fillRect(0, PN532_INFO_Y, LCD_WIDTH, PN532_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(pn532StatusColor);
  canvas.setCursor(20, PN532_INFO_Y + 3);
  canvas.print(pn532StatusMsg);
}

// Nur Sendezaehler + letzter Sendestatus -- wird ausschliesslich vom
// 2s-Sendezyklus in loop() aufgerufen.
void updateSendInfo() {
  canvas.fillRect(0, SEND_INFO_Y, LCD_WIDTH, SEND_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);

  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(20, SEND_INFO_Y + 4);
  canvas.printf("ESP-NOW gesendet: %lu\n", (unsigned long)outgoing.counter);

  canvas.setCursor(20, SEND_INFO_Y + 22);
  if (haveSendResult) {
    canvas.setTextColor(lastSendOk ? TFT_GREENYELLOW : TFT_RED);
    canvas.printf("Letzter Sendestatus: %s\n", lastSendOk ? "OK" : "FEHLER");
  } else {
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.println("Letzter Sendestatus: (noch keiner)");
  }
}

// Nur die zuletzt empfangene Nachricht -- wird ausschliesslich aus
// onDataRecv() (ueber recvPending, siehe loop()) aufgerufen.
void updateReceivedInfo() {
  canvas.fillRect(0, RECV_INFO_Y, LCD_WIDTH, RECV_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_GREENYELLOW);
  canvas.setCursor(20, RECV_INFO_Y + 4);
  if (haveReceived) {
    canvas.printf("Empfangen von %02X:%02X:%02X:%02X:%02X:%02X:\n",
                  lastSrcMac[0], lastSrcMac[1], lastSrcMac[2],
                  lastSrcMac[3], lastSrcMac[4], lastSrcMac[5]);
    canvas.setCursor(20, RECV_INFO_Y + 22);
    canvas.printf("\"%s\" (Zaehler %lu)", lastReceived.text, (unsigned long)lastReceived.counter);
  } else {
    canvas.println("Noch keine ESP-NOW-Nachricht empfangen.");
  }
}

// UID-Zeile + darunter eine kurze DESFire-Zusammenfassung (Details stehen
// in /desfire_log.txt auf der SD-Karte, siehe desfireDeepRead()) -- wird
// ausschliesslich aus loop()s PN532-Polling aufgerufen.
void updateUidInfo() {
  canvas.fillRect(0, UID_INFO_Y, LCD_WIDTH, UID_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_GREENYELLOW);
  canvas.setCursor(20, UID_INFO_Y + 3);
  canvas.print(uidMsg);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setCursor(20, UID_INFO_Y + 26);
  canvas.print(desfireSummaryMsg);
}

// Aktuelle RTC-Zeit als "YYYY-MM-DD hh:mm:ss" -- fuer Log-Zeitstempel.
// Getrennt von rtcFormat() (die haengt zusaetzlich den VL-Hinweis an, den
// wir im Log nicht wollen).
void formatTimestamp(char *buf, size_t bufLen) {
  RtcDateTime dt;
  if (rtcRead(dt)) {
    snprintf(buf, bufLen, "%04u-%02u-%02u %02u:%02u:%02u",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  } else {
    snprintf(buf, bufLen, "????-??-?? ??:??:??");
  }
}

// Wird nur beim Uebergang "nicht gemountet -> gemountet" aufgerufen --
// haengt eine Startmarke an die Log-Datei an (legt sie bei Bedarf an).
void logSessionStart() {
  File f = SD.open(LOG_FILE_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("logSessionStart(): SD.open() fehlgeschlagen");
    return;
  }
  char ts[32];
  formatTimestamp(ts, sizeof(ts));
  f.printf("--- Log gestartet %s ---\n", ts);
  f.close();
  Serial.println("logSessionStart(): Startmarke geschrieben.");
}

// Haengt die zuletzt empfangene ESP-NOW-Nachricht mit RTC-Zeitstempel an
// die Log-Datei an. Wird nur aufgerufen, wenn sdMounted gerade true ist --
// schlaegt der Schreibzugriff trotzdem fehl (Karte just in dem Moment
// herausgezogen), wird das stillschweigend uebersprungen; der naechste
// checkSdCard()-Durchlauf korrigiert den angezeigten Status.
void logReceivedMessage() {
  if (!sdMounted) return;
  File f = SD.open(LOG_FILE_PATH, FILE_APPEND);
  if (!f) return;
  char ts[32];
  formatTimestamp(ts, sizeof(ts));
  f.printf("%s  %02X:%02X:%02X:%02X:%02X:%02X  \"%s\"  Zaehler %lu\n",
           ts, lastSrcMac[0], lastSrcMac[1], lastSrcMac[2],
           lastSrcMac[3], lastSrcMac[4], lastSrcMac[5],
           lastReceived.text, (unsigned long)lastReceived.counter);
  f.close();
}

// Erkennt Einstecken/Herausziehen waehrend des Betriebs, OHNE bei jedem
// Poll-Zyklus einen teuren Vollremount zu machen (siehe firmware/README.md
// Abschnitt 7 -- eine fruehere Version machte das alle 2s unbedingt, was
// bei manchen Karten spuerbar lange blockierte). Solange die Karte als
// gemountet gilt, reicht ein guenstiger Lebendigkeits-Check (Log-Datei kurz
// oeffnen+schliessen, ohne zu schreiben). Zeichnet nur bei einem
// tatsaechlichen Zustandswechsel neu.
void checkSdCard() {
  if (sdMounted) {
    File probe = SD.open(LOG_FILE_PATH, FILE_APPEND);
    if (probe) {
      probe.close();
      return; // weiterhin da, nichts zu tun
    }
    Serial.println("checkSdCard(): Log-Datei nicht mehr erreichbar -- Karte vermutlich entfernt.");
    SD.end();
    sdSpi.end();
    sdMounted = false;
    snprintf(sdStatusMsg, sizeof(sdStatusMsg), "SD-Karte: entfernt");
    sdStatusColor = TFT_RED;
    updateSdInfo();
    return;
  }

  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  bool ok = SD.begin(PIN_SD_CS, sdSpi, 80000000);
  Serial.printf("checkSdCard(): SD.begin() -> %s\n", ok ? "true" : "false");

  if (!ok) {
    if (!sdCheckedOnce) {
      snprintf(sdStatusMsg, sizeof(sdStatusMsg), "SD-Karte: nicht eingesteckt/nicht lesbar");
      sdStatusColor = TFT_LIGHTGREY;
      updateSdInfo();
      sdCheckedOnce = true;
    }
    return;
  }

  sdMounted = true;
  sdCheckedOnce = true;

  uint8_t cardType = SD.cardType();
  const char *typeStr = "unbekannt";
  if (cardType == CARD_MMC) typeStr = "MMC";
  else if (cardType == CARD_SD) typeStr = "SDSC";
  else if (cardType == CARD_SDHC) typeStr = "SDHC";
  snprintf(sdStatusMsg, sizeof(sdStatusMsg), "SD-Karte: erkannt (%s, %llu MB)",
           typeStr, SD.cardSize() / (1024ULL * 1024ULL));
  sdStatusColor = TFT_GREENYELLOW;
  Serial.printf("checkSdCard(): Zustandswechsel -> gemountet, %s\n", sdStatusMsg);
  logSessionStart();
  buzzerBeep(80);
  updateSdInfo();
}

// Einmaliger Verbindungsversuch (siehe pn532Ready oben) -- Firmware-Version
// abfragen und SAM auf normalen Lesemodus konfigurieren.
bool pn532Begin() {
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    snprintf(pn532StatusMsg, sizeof(pn532StatusMsg), "PN532: nicht gefunden (Verkabelung/DIP pruefen)");
    pn532StatusColor = TFT_RED;
    Serial.println("pn532Begin(): getFirmwareVersion() -> 0 (nicht gefunden)");
    return false;
  }
  snprintf(pn532StatusMsg, sizeof(pn532StatusMsg), "PN532: PN5%02X gefunden, Firmware %d.%d",
           (unsigned)((versiondata >> 24) & 0xFF), (unsigned)((versiondata >> 16) & 0xFF),
           (unsigned)((versiondata >> 8) & 0xFF));
  pn532StatusColor = TFT_GREENYELLOW;
  Serial.printf("pn532Begin(): %s\n", pn532StatusMsg);
  nfc.SAMConfig();
  return true;
}

// Fuehrt einen vollstaendigen DESFire-Lesevorgang durch: GetVersion, Liste
// der Anwendungen (AIDs), pro Anwendung Authentifizierung mit dem Default-
// Schluessel (16 Nullbytes, probiert erst 2K3DES dann AES -- siehe
// desfire.h) + Datei-Liste + Dateiinhalte wo moeglich (Enciphered-Dateien
// werden erkannt, aber nicht entschluesselt -- siehe Kopfkommentar in
// desfire.h). Alle Details landen mit RTC-Zeitstempel in DESFIRE_LOG_PATH
// auf der SD-Karte (nur wenn sie gerade gemountet ist); desfireSummaryMsg
// bekommt eine kurze Ein-Zeilen-Zusammenfassung fuers Display.
//
// WICHTIG: Dieser Code wurde an keiner echten DESFire-Karte getestet
// (siehe desfire.h). Schlaegt die Authentifizierung fehl, ist die
// wahrscheinlichste Erklaerung, dass die Karte nicht mehr die
// Werksschluessel hat (normal bei den meisten Karten aus dem echten
// Einsatz) -- kein Grund zur Sorge, kein Bug.
void desfireDeepRead() {
  File log;
  bool haveLog = false;
  if (sdMounted) {
    log = SD.open(DESFIRE_LOG_PATH, FILE_APPEND);
    haveLog = (bool)log;
    if (haveLog) {
      char ts[32];
      formatTimestamp(ts, sizeof(ts));
      log.printf("=== DESFire-Lesevorgang %s ===\n", ts);
    } else {
      Serial.println("desfireDeepRead(): SD.open() fehlgeschlagen, logge nur nach Serial.");
    }
  }

  DesfireVersion ver;
  if (!desfireGetVersion(ver)) {
    snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Kein DESFire (GetVersion fehlgeschlagen)");
    if (haveLog) {
      log.println("GetVersion fehlgeschlagen -- keine DESFire-Karte oder Kommunikationsfehler.");
      log.close();
    }
    return;
  }

  if (haveLog) {
    log.printf("HW: Vendor=0x%02X Type=0x%02X SubType=0x%02X Version=%u.%u StorageSize=0x%02X Protocol=0x%02X\n",
               ver.hwVendorId, ver.hwType, ver.hwSubType, ver.hwMajorVersion, ver.hwMinorVersion,
               ver.hwStorageSize, ver.hwProtocol);
    log.printf("SW: Vendor=0x%02X Type=0x%02X SubType=0x%02X Version=%u.%u StorageSize=0x%02X Protocol=0x%02X\n",
               ver.swVendorId, ver.swType, ver.swSubType, ver.swMajorVersion, ver.swMinorVersion,
               ver.swStorageSize, ver.swProtocol);
    log.printf("UID: %02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
               ver.uid[0], ver.uid[1], ver.uid[2], ver.uid[3], ver.uid[4], ver.uid[5], ver.uid[6]);
    log.printf("Batch: %02X%02X%02X%02X%02X, Produktion: Woche %u / 20%02u\n",
               ver.batchNo[0], ver.batchNo[1], ver.batchNo[2], ver.batchNo[3], ver.batchNo[4],
               ver.productionWeek, ver.productionYear);
  }

  uint8_t aids[28][3];
  uint8_t appCount = desfireGetApplicationIDs(aids, 28);
  if (haveLog) log.printf("Anwendungen (AIDs): %u\n", appCount);

  uint8_t authedApps = 0;
  for (uint8_t a = 0; a < appCount; a++) {
    if (haveLog) log.printf("-- AID %02X%02X%02X --\n", aids[a][2], aids[a][1], aids[a][0]);
    if (!desfireSelectApplication(aids[a])) {
      if (haveLog) log.println("   SelectApplication fehlgeschlagen, ueberspringe.");
      continue;
    }

    uint8_t sessionKey[16];
    const char *cipherName;
    if (!desfireAuthDefaultKey(0, sessionKey, &cipherName)) {
      if (haveLog) log.println("   Authentifizierung mit Default-Schluessel (2K3DES/AES) fehlgeschlagen -- "
                                "Karte hat vermutlich andere Schluessel.");
      continue;
    }
    authedApps++;
    if (haveLog) log.printf("   Authentifiziert mit Default-Schluessel (%s).\n", cipherName);

    uint8_t fileIds[32];
    uint8_t fileCount = desfireGetFileIDs(fileIds, 32);
    if (haveLog) log.printf("   Dateien: %u\n", fileCount);

    for (uint8_t f = 0; f < fileCount; f++) {
      DesfireFileSettings fs;
      if (!desfireGetFileSettings(fileIds[f], fs)) {
        if (haveLog) log.printf("   Datei %u: GetFileSettings fehlgeschlagen.\n", fileIds[f]);
        continue;
      }
      const char *typeName = fs.fileType == 0x00 ? "Standard Data"
                            : fs.fileType == 0x01 ? "Backup Data"
                            : fs.fileType == 0x02 ? "Value"
                            : fs.fileType == 0x03 ? "Linear Record"
                            : fs.fileType == 0x04 ? "Cyclic Record"
                                                   : "Unbekannt";
      const char *commName = fs.commMode == 0x00 ? "Plain" : fs.commMode == 0x01 ? "MACed" : "Enciphered";
      if (haveLog) {
        log.printf("   Datei %u: Typ=%s, Kommunikation=%s", fileIds[f], typeName, commName);
        if (fs.fileType == 0x00 || fs.fileType == 0x01) log.printf(", Groesse=%u Byte", (unsigned)fs.fileSize);
        log.println();
      }

      if (fs.commMode == 0x03) {
        if (haveLog) log.println("     -> Enciphered, wird nicht gelesen (Entschluesselung nicht implementiert).");
        continue;
      }

      uint8_t dataBuf[128];
      uint16_t dataLen = 0;
      if (fs.fileType == 0x00 || fs.fileType == 0x01) {
        dataLen = desfireReadData(fileIds[f], dataBuf, sizeof(dataBuf));
      } else if (fs.fileType == 0x03 || fs.fileType == 0x04) {
        dataLen = desfireReadRecords(fileIds[f], dataBuf, sizeof(dataBuf));
      }
      if (dataLen > 0 && haveLog) {
        log.print("     Inhalt: ");
        for (uint16_t i = 0; i < dataLen; i++) log.printf("%02X ", dataBuf[i]);
        log.println();
        if (fs.commMode == 0x01) log.println("     (MACed -- MAC/CMAC wurde NICHT geprueft, nur Rohdaten gezeigt)");
      }
    }
  }

  if (haveLog) log.close();
  snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "DESFire: %u App(s), %u authentifiziert, %s",
           appCount, authedApps, sdMounted ? "siehe SD-Log" : "SD nicht gemountet");
}

// Core-Versions-abhaengig (siehe README.md Abschnitt 15, Punkt 7).
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
  haveSendResult = true;
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(espnow_message_t)) return;
  memcpy(&lastReceived, data, sizeof(espnow_message_t));
  memcpy(lastSrcMac, info->src_addr, 6);
  haveReceived = true;
  recvPending = true;
}

// Sagt anhand von Julian-Day-artiger Formel den Wochentag voraus (0=So..6=Sa)
uint8_t computeWeekday(uint16_t y, uint8_t m, uint8_t d) {
  static const uint8_t t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y -= 1;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// Parst __DATE__ ("Mmm dd yyyy") und __TIME__ ("hh:mm:ss") und schreibt in die RTC.
void setRtcFromCompileTime() {
  const char *monthNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4];
  int day, year, hour, minute, second;
  sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
  int month = (strstr(monthNames, monStr) - monthNames) / 3 + 1;

  RtcDateTime dt;
  dt.year = year; dt.month = month; dt.day = day;
  dt.hour = hour; dt.minute = minute; dt.second = second;
  dt.weekday = computeWeekday(year, month, day);
  dt.voltageLow = false;

  if (rtcWrite(dt)) {
    Serial.println("RTC auf Compile-Zeit gestellt.");
  } else {
    Serial.println("FEHLER: RTC-Schreibzugriff fehlgeschlagen (Verkabelung/Adresse pruefen).");
  }
}

#ifdef USE_WIFI_NTP_SYNC
// Gibt true/false zurueck, statt nur nach Serial zu loggen -- der
// Touch-Handler (siehe loop()) laesst den NTP-SYNC-Button dafuer kurz rot
// aufleuchten, wenn es NICHT geklappt hat. Ohne diese sichtbare
// Rueckmeldung sah man dem Button nicht an, ob die WLAN-Verbindung
// fehlgeschlagen ist, der NTP-Server nicht erreichbar war, o.ae. --
// man musste den Serial Monitor mitlaufen lassen, um ueberhaupt zu merken,
// DASS es fehlgeschlagen ist.
bool syncFromNtp() {
  Serial.print("Verbinde mit WLAN fuer NTP");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 10000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WLAN-Verbindung fehlgeschlagen, NTP-Sync abgebrochen.");
    return false;
  }

  // EU-Zeitzonenregel (Luxemburg) inkl. automatischer Sommerzeitumstellung.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "europe.pool.ntp.org");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP-Sync fehlgeschlagen (Timeout -- Server nicht erreichbar?).");
    return false;
  }

  RtcDateTime dt;
  dt.year = timeinfo.tm_year + 1900;
  dt.month = timeinfo.tm_mon + 1;
  dt.day = timeinfo.tm_mday;
  dt.hour = timeinfo.tm_hour;
  dt.minute = timeinfo.tm_min;
  dt.second = timeinfo.tm_sec;
  dt.weekday = timeinfo.tm_wday;
  dt.voltageLow = false;

  if (rtcWrite(dt)) {
    Serial.println("RTC per NTP synchronisiert.");
    return true;
  }
  Serial.println("FEHLER: RTC-Schreibzugriff nach NTP-Sync fehlgeschlagen.");
  return false;
}
#endif

void setup() {
  // Muss vor allem anderen laufen (siehe touch_probe.h) -- zwingt den GT911
  // deterministisch auf Adresse 0x5D, statt den Pegel floaten zu lassen.
  gt911PowerOnSequence();

  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 5: PN532 NFC + SD + RTC + WLAN/ESP-NOW -- boot");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);

  // Zusaetzlich zur 120ms-Adress-Auswahl-Sequenz (gt911PowerOnSequence())
  // braucht der GT911-Chip selbst nach einem ECHTEN Stromlos-Start noch
  // Zeit fuer sein eigenes Power-On-Booting (siehe firmware/README.md).
  delay(600);

  uint8_t gtAddr = probeGT911Address();
  touchStandaloneConfig(gtAddr);
  Serial.printf("Touch-Init -> %s\n", touchStandaloneInit() ? "OK" : "FEHLGESCHLAGEN");

  Serial.println("rgbPanelInit() (esp_lcd_panel_rgb, mit Bounce Buffer) ...");
  if (!rgbPanelInit()) {
    Serial.println("Panel-Init fehlgeschlagen -- Sketch haelt an.");
    while (true) { delay(1000); }
  }
  canvas.setColorDepth(16);
  canvas.setBuffer(g_rgbFrameBuffer, LCD_WIDTH, LCD_HEIGHT, 16);

  setBacklightPercent(backlightPercent);

  WiFi.mode(WIFI_STA);

  // Direkt nach WiFi.mode() liefert WiFi.macAddress() auf manchen Boards bei
  // echtem Stromlos-Start noch "00:00:00:00:00:00" -- kurz auf eine echte
  // Adresse warten (siehe firmware/README.md Abschnitt 7).
  {
    uint32_t macWaitStart = millis();
    while (WiFi.macAddress() == "00:00:00:00:00:00" && millis() - macWaitStart < 2000) {
      delay(50);
    }
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("FEHLER: esp_now_init() fehlgeschlagen");
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddr, 6);
  peerInfo.channel = 0;   // aktueller WiFi-Kanal
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("FEHLER: esp_now_add_peer() fehlgeschlagen");
  }

  outgoing.counter = 0;
  snprintf(outgoing.text, sizeof(outgoing.text), "Hallo von %s", WiFi.macAddress().c_str());

  RtcDateTime dt;
  if (rtcRead(dt) && dt.voltageLow) {
    Serial.println("Hinweis: VL-Bit gesetzt -- Zeit nach Spannungsausfall unsicher, bitte stellen.");
  }

  checkSdCard();
  pn532Ready = pn532Begin();
  if (!pn532Ready) {
    buzzerBeep(300);
  }

  drawStaticParts();
  updateSdInfo();
  updateRtcInfo();
  updatePn532Info();
  updateSendInfo();
  updateReceivedInfo();
  updateUidInfo();
  Serial.println("Setup fertig.");
}

uint32_t lastRtcDrawMs = 0;
uint32_t lastSdPollMs = 0;

void loop() {
  // Nur hier, im loop()-Task, wird tatsaechlich gezeichnet/geloggt -- siehe
  // Kommentar bei recvPending oben.
  if (recvPending) {
    recvPending = false;
    updateReceivedInfo();
    logReceivedMessage();
  }

  uint32_t now = millis();
  if (now - lastRtcDrawMs >= RTC_UPDATE_INTERVAL_MS) {
    lastRtcDrawMs = now;
    updateRtcInfo();
  }

  if (now - lastSdPollMs >= SD_POLL_INTERVAL_MS) {
    lastSdPollMs = now;
    checkSdCard();
  }

  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    outgoing.counter++;
    esp_now_send(broadcastAddr, (uint8_t *)&outgoing, sizeof(outgoing));
    updateSendInfo();
  }

  // Kurzer Poll-Versuch (100ms Timeout in readPassiveTargetID()) -- danach
  // 1s Entprellen, damit eine aufliegende Karte nicht staendig neu
  // "erkannt" wird. 100ms statt der urspruenglich probierten 10ms: DESFire-
  // Karten antworten auf die ISO14443A-Anticollision/Select-Sequenz
  // teils spuerbar langsamer als einfache Mifare-Classic-Karten, und
  // Software-SPI (statt Hardware-SPI) fuegt zusaetzliche Kommunikations-
  // Latenz hinzu -- mit 10ms lief das readPassiveTargetID() bei DESFire-
  // Karten offenbar regelmaessig in den Timeout, ohne dass ueberhaupt ein
  // Lesevorgang sichtbar war ("es passiert nichts beim Scannen").
  if (pn532Ready && now - lastPn532AttemptMs >= PN532_POLL_INTERVAL_MS) {
    lastPn532AttemptMs = now;
    if (now - lastUidReadMs >= PN532_DEBOUNCE_MS) {
      uint8_t uid[7];
      uint8_t uidLength;
      if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
        char uidHex[24] = {0};
        for (uint8_t i = 0; i < uidLength; i++) {
          char byteStr[4];
          snprintf(byteStr, sizeof(byteStr), "%02X ", uid[i]);
          strcat(uidHex, byteStr);
        }
        snprintf(uidMsg, sizeof(uidMsg), "Karte erkannt, UID: %s", uidHex);
        buzzerBeep(80);

        // readPassiveTargetID() setzt NICHT die interne _inListedTag-
        // Variable, die inDataExchange() (fuer die DESFire-Kommandos in
        // desfireDeepRead()) braucht -- dafuer muss die Karte zusaetzlich
        // per inListPassiveTarget() aktiviert werden (siehe README.md).
        if (nfc.inListPassiveTarget()) {
          desfireDeepRead();
        } else {
          snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "DESFire: inListPassiveTarget() fehlgeschlagen");
        }
        updateUidInfo();
        lastUidReadMs = now;
      }
    }
  }

  int32_t x, y;
  if (touchStandaloneGetXY(&x, &y)) {
    if (inside(btnSetCompile, x, y)) {
      drawButton(btnSetCompile, TFT_GREEN);
      setRtcFromCompileTime();
      updateRtcInfo();
      buzzerBeep(80);
      drawButton(btnSetCompile, TFT_DARKGREY);
    } else if (inside(btnNtpSync, x, y)) {
      drawButton(btnNtpSync, TFT_GREEN);
      bool ntpOk = false;
#ifdef USE_WIFI_NTP_SYNC
      ntpOk = syncFromNtp();
      updateRtcInfo();
#else
      Serial.println("NTP-Sync deaktiviert -- USE_WIFI_NTP_SYNC einkommentieren und wifi_secrets.h anlegen.");
#endif
      buzzerBeep(80);
      // Sichtbare Rueckmeldung statt nur Serial: rot kurz aufleuchten bei
      // Fehlschlag/deaktiviert, sonst direkt zurueck auf grau.
      drawButton(btnNtpSync, ntpOk ? TFT_DARKGREY : TFT_RED);
      if (!ntpOk) {
        delay(500);
        drawButton(btnNtpSync, TFT_DARKGREY);
      }
    } else if (inside(btnBeep, x, y)) {
      drawButton(btnBeep, TFT_GREEN);
      buzzerBeep(150);
      drawButton(btnBeep, TFT_DARKGREY);
    } else if (inside(btnBrightUp, x, y)) {
      backlightPercent = (backlightPercent <= 90) ? backlightPercent + 10 : 100;
      setBacklightPercent(backlightPercent);
      drawButton(btnBrightUp, TFT_GREEN);
      delay(80);
      drawButton(btnBrightUp, TFT_DARKGREY);
    } else if (inside(btnBrightDn, x, y)) {
      backlightPercent = (backlightPercent >= 10) ? backlightPercent - 10 : 0;
      setBacklightPercent(backlightPercent);
      drawButton(btnBrightDn, TFT_GREEN);
      delay(80);
      drawButton(btnBrightDn, TFT_DARKGREY);
    } else if (y < 352) {
      // Tap auf freie Flaeche -> Hintergrundfarbe wechseln (Panel-Refresh-Test)
      bgIndex = (bgIndex + 1) % (sizeof(bgColors) / sizeof(bgColors[0]));
      drawStaticParts();
      updateSdInfo();
      updateRtcInfo();
      updatePn532Info();
      updateSendInfo();
      updateReceivedInfo();
      updateUidInfo();
    }

    delay(150); // einfaches Debounce
  }
}
