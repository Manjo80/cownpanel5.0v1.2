// Stage 4 -- SD-Karte (TF-Card), Erweiterung von Stage 3
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// Baut auf Stage 3 auf: RTC-Anzeige, STELLEN/NTP-SYNC-Buttons und der
// komplette WLAN/ESP-NOW-Status (MAC, Sendezaehler, Sendestatus, letzte
// empfangene Nachricht) sowie BEEP/BACKLIGHT +/- bleiben erhalten und
// funktionieren weiter. Neu dazu kommt die SD-/TF-Karte:
//  - wird alle 2s neu erkannt (Einstecken/Herausziehen live gemeldet,
//    kein Neustart noetig) -- siehe checkSdCard()
//  - jede empfangene ESP-NOW-Nachricht wird fortlaufend mit RTC-Zeitstempel
//    an /espnow_log.txt angehaengt (nur wenn Karte gerade nutzbar ist)
//
// VORAUSSETZUNG (Hardware-DIP, nicht per Code aenderbar): Function-Select
// S1/S0 = 1/1 ("MIC & TF Card"), siehe README.md Abschnitt 14. In diesem
// Modus fuehren IO4/5/6 zur SD-Karte statt zum Wireless-Header.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <stdio.h>
#include <string.h>
#include "pins.h"
#include "rgb_panel.h"
#include "touch_standalone.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"
#include "rtc_pcf8563.h"

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

struct Button {
  int x, y, w, h;
  const char *label;
};

Button btnSetCompile = { 40,  335, 340, 55, "STELLEN (Compile-Zeit)" };
Button btnNtpSync    = { 420, 335, 340, 55, "NTP-SYNC" };
Button btnBeep       = { 40,  400, 220, 60, "BEEP" };
Button btnBrightUp   = { 300, 400, 220, 60, "BACKLIGHT +" };
Button btnBrightDn   = { 560, 400, 220, 60, "BACKLIGHT -" };

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

uint8_t broadcastAddr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// SD-Status: wird per Vollremount (siehe checkSdCard()) alle 2s neu
// ermittelt -- so faellt Einstecken/Herausziehen live auf, ohne dass die
// Karte beim Booten schon gesteckt haben muss.
bool sdMounted = false;
bool sdCheckedOnce = false;
char sdStatusMsg[64] = "SD-Karte: wird geprueft...";
uint16_t sdStatusColor = TFT_LIGHTGREY;

// Bereichsgrenzen der einzelnen Update-Funktionen -- an einer Stelle
// definiert, damit das fillRect() in der jeweiligen Funktion garantiert
// mit sich selbst konsistent bleibt.
const int SD_INFO_Y = 120, SD_INFO_H = 32;
const int RTC_INFO_Y = 158, RTC_INFO_H = 38;
const int SEND_INFO_Y = 200, SEND_INFO_H = 55;
const int RECV_INFO_Y = 260, RECV_INFO_H = 60;

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
  canvas.println("Stage 4: SD-Karte + RTC + WLAN/ESP-NOW");

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
  canvas.setCursor(20, SD_INFO_Y + 5);
  canvas.print(sdStatusMsg);
}

// Nur die RTC-Zeile -- wird jede Sekunde aus loop() aufgerufen, unabhaengig
// von SD-/ESP-NOW-Updates (eigener Bildschirmbereich).
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

// Nur Sendezaehler + letzter Sendestatus -- wird ausschliesslich vom
// 2s-Sendezyklus in loop() aufgerufen.
void updateSendInfo() {
  canvas.fillRect(0, SEND_INFO_Y, LCD_WIDTH, SEND_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);

  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(20, SEND_INFO_Y + 8);
  canvas.printf("ESP-NOW gesendet: %lu\n", (unsigned long)outgoing.counter);

  canvas.setCursor(20, SEND_INFO_Y + 32);
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
  canvas.setCursor(20, RECV_INFO_Y + 5);
  if (haveReceived) {
    canvas.printf("Empfangen von %02X:%02X:%02X:%02X:%02X:%02X:\n",
                  lastSrcMac[0], lastSrcMac[1], lastSrcMac[2],
                  lastSrcMac[3], lastSrcMac[4], lastSrcMac[5]);
    canvas.setCursor(20, RECV_INFO_Y + 30);
    canvas.printf("\"%s\" (Zaehler %lu)", lastReceived.text, (unsigned long)lastReceived.counter);
  } else {
    canvas.println("Noch keine ESP-NOW-Nachricht empfangen.");
  }
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
// Poll-Zyklus einen teuren Vollremount zu machen (fruehere Version machte
// das alle 2s unbedingt -- SD.begin() kann bei manchen Karten spuerbar
// lange blockieren, was den kompletten loop()-Task fuer diese Zeit anhielt
// und im schlimmsten Fall dazu fuehrte, dass die Statuszeile nie zum
// Zeichnen kam).
//
// Solange die Karte als gemountet gilt, reicht ein GUENSTIGER Lebendig-
// keits-Check (Log-Datei kurz oeffnen+schliessen, ohne zu schreiben) --
// erst wenn DER fehlschlaegt, wird sauber zurueckgesetzt und beim naechsten
// Durchlauf ein neuer Mount-Versuch gestartet. Zeichnet nur bei einem
// tatsaechlichen Zustandswechsel neu (gleiches Minimal-Redraw-Prinzip wie
// bei den ESP-NOW-Updates).
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
void syncFromNtp() {
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
    return;
  }

  // EU-Zeitzonenregel (Luxemburg) inkl. automatischer Sommerzeitumstellung.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "europe.pool.ntp.org");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP-Sync fehlgeschlagen (Timeout).");
    return;
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
  } else {
    Serial.println("FEHLER: RTC-Schreibzugriff nach NTP-Sync fehlgeschlagen.");
  }
}
#endif

void setup() {
  // Muss vor allem anderen laufen (siehe touch_probe.h) -- zwingt den GT911
  // deterministisch auf Adresse 0x5D, statt den Pegel floaten zu lassen.
  gt911PowerOnSequence();

  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 4: SD-Karte + RTC + WLAN/ESP-NOW -- boot");

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

  drawStaticParts();
  updateSdInfo();
  updateRtcInfo();
  updateSendInfo();
  updateReceivedInfo();
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
#ifdef USE_WIFI_NTP_SYNC
      syncFromNtp();
      updateRtcInfo();
#else
      Serial.println("NTP-Sync deaktiviert -- USE_WIFI_NTP_SYNC einkommentieren und wifi_secrets.h anlegen.");
#endif
      buzzerBeep(80);
      drawButton(btnNtpSync, TFT_DARKGREY);
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
    } else if (y < 330) {
      // Tap auf freie Flaeche -> Hintergrundfarbe wechseln (Panel-Refresh-Test)
      bgIndex = (bgIndex + 1) % (sizeof(bgColors) / sizeof(bgColors[0]));
      drawStaticParts();
      updateSdInfo();
      updateRtcInfo();
      updateSendInfo();
      updateReceivedInfo();
    }

    delay(150); // einfaches Debounce
  }
}
