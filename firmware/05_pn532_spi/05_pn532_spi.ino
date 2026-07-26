// Stage 5 -- PN532 NFC ueber (Software-)SPI, Funktionstest
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// VERKABELUNG (siehe README.md Abschnitt 10.2 -- Pins aus zwei Headern
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
//   Werkzeuge -> USB CDC On Boot: Enabled
// Debug-Text erscheint zusaetzlich auf dem Display, falls UART0 durch den
// PN532 gestoert wird.
//
// BENOETIGTE LIBRARY: Adafruit_PN532 (Library Manager), MIT ZWEI PATCHES
// (siehe README.md Abschnitt 1 + 10.3), sonst schlagen laengere Antworten
// (z. B. GetFirmwareVersion bei manchen Modulklonen, DESFire-APDUs) fehl:
//   1. In Adafruit_PN532.h:  #define PN532_PACKBUFFSIZ 64   ->  255
//   2. In Adafruit_PN532.cpp, inDataExchange(): Timeout 1000 -> 5000 (ms)
//
// Dieser Test deckt NUR die Basis-Verbindung ab (Firmware-Version +
// UID-Read per readPassiveTargetID()). Fuer den vollen DESFire-Lebenszyklus
// (AES-Auth, CreateApplication, Credit/Debit, ...) wird stattdessen
// inListPassiveTarget() zur Kartenaktivierung gebraucht (setzt _inListedTag,
// Voraussetzung fuer inDataExchange()) -- siehe README.md Abschnitte 10-12,
// bewusst nicht Teil dieses Basis-Funktionstests.

#include <Wire.h>
#include <SPI.h>
#include <string.h>
#include <Adafruit_PN532.h>
#include "pins.h"
#include "LGFX_Driver.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"

LGFX lcd;

// Software-SPI-Konstruktor: (SCK, MISO, MOSI, SS) -- vier frei gewaehlte
// Pins, kein Hardware-SPI-Block, siehe README.md Abschnitt 10.2.
Adafruit_PN532 nfc(PIN_PN532_SCK, PIN_PN532_MISO, PIN_PN532_MOSI, PIN_PN532_SS);

int consoleY = 100;
const int lineHeight = 26;

void consolePrint(const char *msg, uint16_t color = TFT_WHITE) {
  Serial.println(msg);
  lcd.setTextColor(color);
  lcd.setTextSize(2);
  lcd.setCursor(20, consoleY);
  lcd.println(msg);
  consoleY += lineHeight;
  if (consoleY > LCD_HEIGHT - lineHeight) {
    lcd.fillRect(0, 90, LCD_WIDTH, LCD_HEIGHT - 90, TFT_BLACK);
    consoleY = 100;
  }
}

bool pn532Begin() {
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    return false;
  }
  char line[64];
  snprintf(line, sizeof(line), "PN5%02X gefunden, Firmware %d.%d",
           (versiondata >> 24) & 0xFF, (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);
  consolePrint(line, TFT_GREEN);

  // SAM-Konfiguration: normaler Lesemodus, kein Secure Access Module.
  nfc.SAMConfig();
  return true;
}

void setup() {
  // Muss vor allem anderen laufen (siehe touch_probe.h) -- zwingt den GT911
  // deterministisch auf Adresse 0x5D, statt den Pegel floaten zu lassen.
  gt911PowerOnSequence();

  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 5: PN532 SPI -- boot");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);
  uint8_t gtAddr = probeGT911Address();
  lcd.setTouchAddr(gtAddr);
  lcd.init();
  lcd.initDMA();
  lcd.startWrite();
  setBacklightPercent(80);

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(lgfx::top_left);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(3);
  lcd.setCursor(20, 20);
  lcd.println("Stage 5: PN532 (SPI)");
  lcd.setTextSize(2);
  lcd.setCursor(20, 60);
  lcd.println("Karte auf den Leser legen ...");

  if (!pn532Begin()) {
    consolePrint("FEHLER: PN532 nicht gefunden.", TFT_RED);
    consolePrint("Pruefen: Verkabelung, DIP=SPI, PACKBUFFSIZ-Patch.", TFT_RED);
    buzzerBeep(300);
  }
}

void loop() {
  static uint32_t lastAttemptMs = 0;
  if (millis() - lastAttemptMs < 300) return;
  lastAttemptMs = millis();

  uint8_t uid[7];
  uint8_t uidLength;

  // Fuer reines UID-Lesen (dieser Basistest) reicht readPassiveTargetID().
  // ACHTUNG: readPassiveTargetID() setzt NICHT die interne _inListedTag-
  // Variable -- sobald darauf aufbauend inDataExchange()-Kommandos noetig
  // sind (z. B. DESFire-APDUs), muss stattdessen inListPassiveTarget()
  // zur Kartenaktivierung verwendet werden. Siehe README.md Abschnitt 10.3.
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 10)) {
    char line[64];
    char uidHex[24] = {0};
    for (uint8_t i = 0; i < uidLength; i++) {
      char byteStr[4];
      snprintf(byteStr, sizeof(byteStr), "%02X ", uid[i]);
      strcat(uidHex, byteStr);
    }
    snprintf(line, sizeof(line), "Karte erkannt, UID: %s", uidHex);
    consolePrint(line, TFT_GREENYELLOW);
    buzzerBeep(80);
    delay(1000); // Entprellen, verhindert Dauerfeuer bei aufliegender Karte
  }
}
