// Stage 2 -- WLAN + ESP-NOW Funktionstest, Erweiterung von Stage 1
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// Baut auf Stage 1 auf: dieselben drei Buttons (BEEP, BACKLIGHT +/-) bleiben
// erhalten und funktionieren weiter -- so sieht man auf einen Blick, dass
// Display/Touch/Buzzer aus Stage 1 durch die neuen WLAN/ESP-NOW-Funktionen
// nicht kaputtgegangen sind. ALLE Statusdaten (eigene MAC-Adresse,
// ESP-NOW-Sendezaehler, letzter Sendestatus, letzte empfangene Nachricht)
// werden direkt auf dem Display angezeigt -- kein Serial Monitor noetig.
//
// ESP-NOW braucht KEINE AP-Verbindung -- WIFI_STA-Modus ohne WiFi.begin()
// reicht. Zum Testen zwei Boards mit demselben Sketch flashen: jedes sendet
// alle 2s einen Broadcast und zeigt eingehende Nachrichten des jeweils
// anderen Boards an (+ Buzzer-Beep bei Empfang).

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_heap_caps.h>
#include "pins.h"
#include "rgb_panel.h"
#include "touch_standalone.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"

// Optionale echte WLAN-Verbindung (nur fuer spaeteres NTP, siehe Stage 3).
// #define USE_WIFI_STA_CONNECT
#ifdef USE_WIFI_STA_CONNECT
#include "wifi_secrets.h"  // Kopie von wifi_secrets.example.h mit echten Daten
#endif

// canvas zeigt auf einen EIGENEN PSRAM-Puffer (nicht auf einen der beiden
// Hardware-Framebuffer von rgbPanelInit()) -- die GDMA sieht ihn nie, daher
// ist beliebig granulares Zeichnen hier tearing-frei. rgbPanelFlush() nach
// jeder fertigen Aenderung kopiert den Inhalt tearing-frei in die Hardware
// (siehe Erklaerung in rgb_panel.h).
LGFX_Sprite canvas;
static uint16_t *g_canvasBuffer = nullptr;

struct Button {
  int x, y, w, h;
  const char *label;
};

Button btnBeep      = { 40,  400, 220, 60, "BEEP" };
Button btnBrightUp  = { 300, 400, 220, 60, "BACKLIGHT +" };
Button btnBrightDn  = { 560, 400, 220, 60, "BACKLIGHT -" };

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
const uint32_t SEND_INTERVAL_MS = 2000;

uint8_t broadcastAddr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

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

// Titel + eigene MAC-Adresse + Buttons -- aendert sich nur, wenn sich die
// Hintergrundfarbe aendert (Tap auf freie Flaeche). Die MAC-Adresse steht
// bereits vor esp_now_init() fest und aendert sich zur Laufzeit nie, daher
// gehoert sie hierher statt in eine der beiden Update-Funktionen unten.
void drawStaticParts() {
  canvas.fillScreen(bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(3);
  canvas.setCursor(20, 20);
  canvas.println("CrowPanel Advance 5.0");
  canvas.setTextSize(2);
  canvas.println("Stage 2: Display+Touch+Buzzer + WLAN/ESP-NOW");

  canvas.setCursor(20, 90);
  canvas.print("Eigene MAC: ");
  canvas.println(WiFi.macAddress());

  drawButton(btnBeep, TFT_DARKGREY);
  drawButton(btnBrightUp, TFT_DARKGREY);
  drawButton(btnBrightDn, TFT_DARKGREY);
}

// Bereichsgrenzen der beiden Update-Funktionen -- an einer Stelle definiert,
// damit das fillRect() hier drin und der zugehoerige rgbPanelFlushRect()-
// Aufruf in loop()/onDataRecv() garantiert denselben Bereich benutzen.
const int SEND_INFO_Y = 120, SEND_INFO_H = 70;
const int RECV_INFO_Y = 195, RECV_INFO_H = 185;

// Nur Sendezaehler + letzter Sendestatus -- wird ausschliesslich vom
// 2s-Sendezyklus in loop() aufgerufen, NICHT bei jedem Empfang (dafuer
// gibt es updateReceivedInfo() unten, mit eigenem Bildschirmbereich).
void updateSendInfo() {
  canvas.fillRect(0, SEND_INFO_Y, LCD_WIDTH, SEND_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);

  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(20, 130);
  canvas.printf("ESP-NOW gesendet: %lu\n", (unsigned long)outgoing.counter);

  canvas.setCursor(20, 160);
  if (haveSendResult) {
    canvas.setTextColor(lastSendOk ? TFT_GREENYELLOW : TFT_RED);
    canvas.printf("Letzter Sendestatus: %s\n", lastSendOk ? "OK" : "FEHLER");
  } else {
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.println("Letzter Sendestatus: (noch keiner)");
  }
}

// Nur die zuletzt empfangene Nachricht -- wird ausschliesslich aus
// onDataRecv() aufgerufen, NICHT beim eigenen Senden.
void updateReceivedInfo() {
  canvas.fillRect(0, RECV_INFO_Y, LCD_WIDTH, RECV_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_GREENYELLOW);
  canvas.setCursor(20, 200);
  if (haveReceived) {
    canvas.printf("Empfangen von %02X:%02X:%02X:%02X:%02X:%02X:\n",
                  lastSrcMac[0], lastSrcMac[1], lastSrcMac[2],
                  lastSrcMac[3], lastSrcMac[4], lastSrcMac[5]);
    canvas.setCursor(20, 230);
    canvas.printf("\"%s\" (Zaehler %lu)", lastReceived.text, (unsigned long)lastReceived.counter);
  } else {
    canvas.println("Noch keine ESP-NOW-Nachricht empfangen.");
  }
}

// Core-Versions-abhaengig (siehe README.md Abschnitt 15, Punkt 7): dieser
// PlatformIO-Toolchain-Stand erwartet fuer esp_now_send_cb_t
// wifi_tx_info_t* statt des aelteren uint8_t* mac_addr. Bei einem
// Core-Wechsel ggf. wieder auf "const uint8_t *mac_addr" zuruecksetzen.
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
  haveSendResult = true;
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(espnow_message_t)) return;
  memcpy(&lastReceived, data, sizeof(espnow_message_t));
  memcpy(lastSrcMac, info->src_addr, 6);
  haveReceived = true;
  buzzerBeep(60);
  updateReceivedInfo();
  rgbPanelFlushRect(g_canvasBuffer, 0, RECV_INFO_Y, LCD_WIDTH, RECV_INFO_H);
}

void setup() {
  // Muss vor allem anderen laufen (siehe touch_probe.h) -- zwingt den GT911
  // deterministisch auf Adresse 0x5D, statt den Pegel floaten zu lassen.
  gt911PowerOnSequence();

  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 2: WLAN + ESP-NOW -- boot");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);

  // Zusaetzlich zur 120ms-Adress-Auswahl-Sequenz (gt911PowerOnSequence())
  // braucht der GT911-Chip selbst nach einem ECHTEN Stromlos-Start noch
  // Zeit fuer sein eigenes Power-On-Booting.
  delay(600);

  uint8_t gtAddr = probeGT911Address();
  touchStandaloneConfig(gtAddr);
  Serial.printf("Touch-Init -> %s\n", touchStandaloneInit() ? "OK" : "FEHLGESCHLAGEN");

  Serial.println("rgbPanelInit() (esp_lcd_panel_rgb, Doppelpuffer) ...");
  if (!rgbPanelInit()) {
    Serial.println("Panel-Init fehlgeschlagen -- Sketch haelt an.");
    while (true) { delay(1000); }
  }

  // Eigener PSRAM-Puffer fuer canvas (siehe rgb_panel.h) -- NICHT einer der
  // beiden Hardware-Framebuffer, die esp_lcd_panel_rgb intern verwaltet.
  g_canvasBuffer = (uint16_t *)heap_caps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (g_canvasBuffer == nullptr) {
    Serial.println("Canvas-Speicher (PSRAM) konnte nicht allokiert werden -- Sketch haelt an.");
    while (true) { delay(1000); }
  }
  canvas.setColorDepth(16);
  canvas.setBuffer(g_canvasBuffer, LCD_WIDTH, LCD_HEIGHT, 16);

  setBacklightPercent(backlightPercent);

  WiFi.mode(WIFI_STA);
#ifdef USE_WIFI_STA_CONNECT
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Verbinde mit WLAN");
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 10000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WLAN-Verbindung fehlgeschlagen -- ESP-NOW funktioniert trotzdem.");
  }
#endif

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

  drawStaticParts();
  updateSendInfo();
  updateReceivedInfo();
  rgbPanelFlush(g_canvasBuffer);
  Serial.println("Setup fertig.");
}

void loop() {
  uint32_t now = millis();
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    outgoing.counter++;
    esp_now_send(broadcastAddr, (uint8_t *)&outgoing, sizeof(outgoing));
    updateSendInfo();
    rgbPanelFlushRect(g_canvasBuffer, 0, SEND_INFO_Y, LCD_WIDTH, SEND_INFO_H);
  }

  int32_t x, y;
  if (touchStandaloneGetXY(&x, &y)) {
    if (inside(btnBeep, x, y)) {
      drawButton(btnBeep, TFT_GREEN);
      rgbPanelFlushRect(g_canvasBuffer, btnBeep.x, btnBeep.y, btnBeep.w, btnBeep.h);
      buzzerBeep(150);
      drawButton(btnBeep, TFT_DARKGREY);
      rgbPanelFlushRect(g_canvasBuffer, btnBeep.x, btnBeep.y, btnBeep.w, btnBeep.h);
    } else if (inside(btnBrightUp, x, y)) {
      backlightPercent = (backlightPercent <= 90) ? backlightPercent + 10 : 100;
      setBacklightPercent(backlightPercent);
      drawButton(btnBrightUp, TFT_GREEN);
      rgbPanelFlushRect(g_canvasBuffer, btnBrightUp.x, btnBrightUp.y, btnBrightUp.w, btnBrightUp.h);
      delay(80);
      drawButton(btnBrightUp, TFT_DARKGREY);
      rgbPanelFlushRect(g_canvasBuffer, btnBrightUp.x, btnBrightUp.y, btnBrightUp.w, btnBrightUp.h);
    } else if (inside(btnBrightDn, x, y)) {
      backlightPercent = (backlightPercent >= 10) ? backlightPercent - 10 : 0;
      setBacklightPercent(backlightPercent);
      drawButton(btnBrightDn, TFT_GREEN);
      rgbPanelFlushRect(g_canvasBuffer, btnBrightDn.x, btnBrightDn.y, btnBrightDn.w, btnBrightDn.h);
      delay(80);
      drawButton(btnBrightDn, TFT_DARKGREY);
      rgbPanelFlushRect(g_canvasBuffer, btnBrightDn.x, btnBrightDn.y, btnBrightDn.w, btnBrightDn.h);
    } else if (y < 380) {
      bgIndex = (bgIndex + 1) % (sizeof(bgColors) / sizeof(bgColors[0]));
      drawStaticParts();
      updateSendInfo();
      updateReceivedInfo();
      rgbPanelFlush(g_canvasBuffer);
    }

    delay(150); // einfaches Debounce
  }
}
