// Stage 4 -- SD-Karte (TF-Card) Funktionstest
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// VORAUSSETZUNG (Hardware-DIP, nicht per Code aenderbar): Function-Select
// S1/S0 = 1/1 ("MIC & TF Card"), siehe README.md Abschnitt 14. In diesem
// Modus fuehren IO4/5/6 zur SD-Karte statt zum Wireless-Header.
//
// Testet: Karteninit, Kartengroesse/-typ, Root-Verzeichnisauflistung,
// Schreiben+Zurücklesen einer Testdatei. Ergebnis erscheint auf dem Display
// und ueber Serial.

#include <Wire.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include "pins.h"
#include "LGFX_Driver.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"

LGFX lcd;
SPIClass sdSpi(HSPI);

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

void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  char line[128];
  File root = fs.open(dirname);
  if (!root || !root.isDirectory()) {
    consolePrint("Root-Verzeichnis nicht lesbar", TFT_RED);
    return;
  }
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      snprintf(line, sizeof(line), "  DIR : %s", file.name());
      consolePrint(line);
      if (levels) listDir(fs, file.path(), levels - 1);
    } else {
      snprintf(line, sizeof(line), "  FILE: %-20s %6u B", file.name(), (unsigned)file.size());
      consolePrint(line);
    }
    file = root.openNextFile();
  }
}

bool sdInit() {
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  return SD.begin(PIN_SD_CS, sdSpi, 80000000);
}

void writeReadTest() {
  const char *path = "/crowpanel_test.txt";
  const char *payload = "CrowPanel Advance 5.0 SD-Test";

  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    consolePrint("Schreiben fehlgeschlagen (open)", TFT_RED);
    return;
  }
  f.println(payload);
  f.close();

  f = SD.open(path, FILE_READ);
  if (!f) {
    consolePrint("Lesen fehlgeschlagen (open)", TFT_RED);
    return;
  }
  String readBack = f.readStringUntil('\n');
  f.close();

  if (readBack == payload) {
    consolePrint("Schreib/Lese-Test: OK", TFT_GREEN);
  } else {
    consolePrint("Schreib/Lese-Test: Inhalt weicht ab!", TFT_RED);
  }
}

void setup() {
  // Muss vor allem anderen laufen (siehe touch_probe.h) -- zwingt den GT911
  // deterministisch auf Adresse 0x5D, statt den Pegel floaten zu lassen.
  gt911PowerOnSequence();

  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 4: SD-Karte -- boot");

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
  lcd.println("Stage 4: SD-Karte");
  lcd.setTextSize(2);
  lcd.setCursor(20, 60);
  lcd.println("(DIP S1/S0 muss auf 1/1 stehen)");

  if (!sdInit()) {
    consolePrint("FEHLER: SD-Karte nicht gefunden.", TFT_RED);
    consolePrint("Pruefen: DIP S1/S0=1/1, Karte eingesteckt, FAT32.", TFT_RED);
    buzzerBeep(300);
    return;
  }

  uint8_t cardType = SD.cardType();
  const char *typeStr = "UNBEKANNT";
  if (cardType == CARD_MMC) typeStr = "MMC";
  else if (cardType == CARD_SD) typeStr = "SDSC";
  else if (cardType == CARD_SDHC) typeStr = "SDHC";

  char line[64];
  snprintf(line, sizeof(line), "Kartentyp: %s", typeStr);
  consolePrint(line, TFT_GREEN);
  snprintf(line, sizeof(line), "Groesse: %llu MB", SD.cardSize() / (1024ULL * 1024ULL));
  consolePrint(line, TFT_GREEN);

  consolePrint("-- Inhalt / --");
  listDir(SD, "/", 1);

  consolePrint("-- Schreib/Lese-Test --");
  writeReadTest();

  buzzerBeep(80);
}

void loop() {
  // Tippen auf den Bildschirm wiederholt Listing + Schreib/Lese-Test.
  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    lcd.fillRect(0, 90, LCD_WIDTH, LCD_HEIGHT - 90, TFT_BLACK);
    consoleY = 100;
    consolePrint("-- erneuter Test --");
    if (sdInit()) {
      listDir(SD, "/", 1);
      writeReadTest();
    } else {
      consolePrint("FEHLER: SD-Karte nicht gefunden.", TFT_RED);
    }
    delay(300);
  }
}
