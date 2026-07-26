// Stage 2 -- WLAN + ESP-NOW Funktionstest
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// ESP-NOW braucht KEINE AP-Verbindung -- WIFI_STA-Modus ohne WiFi.begin()
// reicht. Zum Testen zwei Boards mit demselben Sketch flashen: jedes sendet
// alle 2s einen Broadcast und zeigt eingehende Nachrichten des jeweils
// anderen Boards auf dem Display an (+ Buzzer-Beep bei Empfang).
//
// WICHTIG (siehe README.md Abschnitt 15, Punkt 7): Die Callback-Signaturen
// von esp_now_register_recv_cb/send_cb sind Core-Versions-abhaengig.
// Dieser Code ist auf ESP32-Arduino-Core 3.x (esp_now_recv_info_t*) geschrieben,
// entsprechend dem in README.md Abschnitt 1 dokumentierten Core 3.3.11.
// Auf Core 2.x muss der recv-Callback stattdessen
// void(const uint8_t *mac_addr, const uint8_t *data, int len) lauten.

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include "pins.h"
#include "LGFX_Driver.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"

// Optionale echte WLAN-Verbindung (nur fuer spaeteres NTP, siehe Stage 3).
// Auskommentiert lassen, wenn nur ESP-NOW getestet werden soll.
// #define USE_WIFI_STA_CONNECT
#ifdef USE_WIFI_STA_CONNECT
#include "wifi_secrets.h"  // Kopie von wifi_secrets.example.h mit echten Daten
#endif

LGFX lcd;

typedef struct __attribute__((packed)) {
  char     text[48];
  uint32_t counter;
} espnow_message_t;

espnow_message_t outgoing;
espnow_message_t lastReceived;
bool haveReceived = false;
uint32_t lastSendMs = 0;
const uint32_t SEND_INTERVAL_MS = 2000;

uint8_t broadcastAddr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void redrawScreen() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(lgfx::top_left);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(3);
  lcd.setCursor(20, 20);
  lcd.println("Stage 2: WLAN + ESP-NOW");

  lcd.setTextSize(2);
  lcd.setCursor(20, 80);
  lcd.print("Eigene MAC: ");
  lcd.println(WiFi.macAddress());

  lcd.setCursor(20, 120);
  lcd.printf("Gesendet: %lu\n", (unsigned long)outgoing.counter);

  lcd.setCursor(20, 180);
  lcd.setTextColor(TFT_GREENYELLOW);
  if (haveReceived) {
    lcd.printf("Letzte Nachricht:\n\"%s\"\n(Zaehler %lu)",
               lastReceived.text, (unsigned long)lastReceived.counter);
  } else {
    lcd.println("Noch keine Nachricht empfangen.");
  }
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.printf("ESP-NOW send status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FEHLER");
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(espnow_message_t)) return;
  memcpy(&lastReceived, data, sizeof(espnow_message_t));
  haveReceived = true;
  Serial.printf("ESP-NOW empfangen von %02X:%02X:%02X:%02X:%02X:%02X: \"%s\" (%lu)\n",
                info->src_addr[0], info->src_addr[1], info->src_addr[2],
                info->src_addr[3], info->src_addr[4], info->src_addr[5],
                lastReceived.text, (unsigned long)lastReceived.counter);
  buzzerBeep(60);
  redrawScreen();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 2: WLAN + ESP-NOW -- boot");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);
  uint8_t gtAddr = probeGT911Address();
  lcd.setTouchAddr(gtAddr);
  lcd.init();
  lcd.initDMA();
  lcd.startWrite();
  setBacklightPercent(80);

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
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Verbunden, IP: %s, Kanal: %d\n",
                   WiFi.localIP().toString().c_str(), WiFi.channel());
  } else {
    Serial.println("WLAN-Verbindung fehlgeschlagen -- ESP-NOW funktioniert trotzdem.");
  }
#endif

  Serial.printf("Eigene MAC-Adresse: %s\n", WiFi.macAddress().c_str());

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

  redrawScreen();
  Serial.println("Setup fertig.");
}

void loop() {
  uint32_t now = millis();
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    outgoing.counter++;
    esp_err_t result = esp_now_send(broadcastAddr, (uint8_t *)&outgoing, sizeof(outgoing));
    if (result != ESP_OK) {
      Serial.printf("esp_now_send() Fehler: %d\n", result);
    }
    redrawScreen();
  }

  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    buzzerBeep(40); // Touch bleibt in diesem Stage nur als Lebenszeichen nutzbar
    delay(150);
  }
}
