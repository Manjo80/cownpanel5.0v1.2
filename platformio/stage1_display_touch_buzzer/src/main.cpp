// Stage 1 -- Display + Touch (GT911) + Buzzer/Backlight Funktionstest
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// PlatformIO-Portierung von firmware/01_display_touch_buzzer -- identischer
// Code, aber mit korrekt auf 120 MHz getaktetem PSRAM (siehe platformio.ini),
// im Gegensatz zum Arduino-IDE-Standard-Core (dort fest auf 80 MHz).
//
// Test deckt ab:
//  - RGB-Panel-Bringup ueber LovyanGFX (Bus_RGB/Panel_RGB)
//  - GT911-Touch mit Boot-zu-Boot-Adresswechsel-Fix (0x5D/0x14 Sondierung)
//  - Backlight-Helligkeit und Buzzer ueber STC8H1K28 (I2C 0x30)
//
// Bedienung: Antippen wechselt die Hintergrundfarbe. Die drei Buttons unten
// testen Buzzer und Backlight-Stufen.

#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include "esp_private/periph_ctrl.h"
#include "sdkconfig.h"
#include "pins.h"
#include "LGFX_Driver.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"

LGFX lcd;

// Off-Screen-Puffer fuer Buttons: LovyanGFX bietet fuer RGB-Panels KEINEN
// Tearing-Schutz (Panel_RGB::waitDisplay() ist ein Leerlauf-Stub -- kein
// Vsync-Warten, kein Doppelpuffer). Ein Button direkt in mehreren Schritten
// (Fuellung, Rahmen, Text) auf den live gescannten Framebuffer zu zeichnen
// kann daher als halbfertiger Zwischenstand sichtbar werden ("zerrissenes"
// Bild/Streifen). Fix: Button komplett unsichtbar in einem Sprite aufbauen
// und erst als EIN fertiges Bild per pushSprite() auf den Schirm schreiben.
LGFX_Sprite btnSprite(&lcd);

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

void drawButton(const Button &b, uint16_t fill) {
  // WICHTIG: pushSprite() stoesst die Uebertragung per DMA an und kehrt
  // sofort zurueck, OHNE auf deren Abschluss zu warten. btnSprite ist ein
  // einziger, wiederverwendeter Puffer fuer alle drei Buttons -- ohne
  // waitDMA() wuerde der naechste fillSprite()-Aufruf denselben Speicher
  // ueberschreiben, waehrend die vorherige Uebertragung noch laeuft, und
  // dabei zerstoerte/vermischte Pixel auf dem Schirm hinterlassen
  // (genau das beobachtete Streifen-/Rauschmuster auf den Buttons).
  lcd.waitDMA();
  btnSprite.fillSprite(bgColors[bgIndex]);
  btnSprite.fillRoundRect(0, 0, b.w, b.h, 8, fill);
  btnSprite.drawRoundRect(0, 0, b.w, b.h, 8, TFT_WHITE);
  btnSprite.setTextColor(TFT_WHITE);
  btnSprite.setTextSize(2);
  btnSprite.setTextDatum(lgfx::middle_center);
  btnSprite.drawString(b.label, b.w / 2, b.h / 2);
  btnSprite.pushSprite(b.x, b.y);
  lcd.waitDMA();
}

bool inside(const Button &b, int32_t x, int32_t y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

void drawStaticUI() {
  lcd.fillScreen(bgColors[bgIndex]);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(3);
  lcd.setTextDatum(lgfx::top_left);
  lcd.setCursor(20, 20);
  lcd.println("CrowPanel Advance 5.0");
  lcd.setTextSize(2);
  lcd.println("Stage 1: Display + Touch + Buzzer (PlatformIO/120MHz)");
  drawButton(btnBeep, TFT_DARKGREY);
  drawButton(btnBrightUp, TFT_DARKGREY);
  drawButton(btnBrightDn, TFT_DARKGREY);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 1: Display + Touch + Buzzer -- boot");

  // Objektiver Reset-Grund statt "Reset-Knopf gedrueckt" nach Gefuehl --
  // zeigt z. B. POWERON_RESET (Stromlos) vs. SW_CPU_RESET/USB_UART_CHIP_RESET
  // (Reset-Knopf/Serial-Monitor-Autoreset).
  Serial.printf("Reset-Grund: %d\n", (int)esp_reset_reason());

  // PSRAM-Takt, mit dem dieser Build kompiliert wurde -- muss hier "120 MHz"
  // zeigen (im Gegensatz zum Arduino-IDE-Standard-Core, der 80 MHz meldet).
#if defined(CONFIG_SPIRAM_SPEED_120M)
  Serial.println("PSRAM-Takt (kompiliert): 120 MHz");
#elif defined(CONFIG_SPIRAM_SPEED_80M)
  Serial.println("PSRAM-Takt (kompiliert): 80 MHz  <-- evtl. Diskrepanz zur Hardware!");
#elif defined(CONFIG_SPIRAM_SPEED_40M)
  Serial.println("PSRAM-Takt (kompiliert): 40 MHz  <-- evtl. Diskrepanz zur Hardware!");
#else
  Serial.println("PSRAM-Takt (kompiliert): unbekannt (kein CONFIG_SPIRAM_SPEED_* Makro gesetzt)");
#endif

  Serial.printf("PSRAM gesamt: %u Bytes, frei: %u Bytes\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  if (ESP.getPsramSize() == 0) {
    Serial.println("WARNUNG: Kein PSRAM erkannt!");
  }

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);

  uint8_t gtAddr = probeGT911Address();
  Serial.printf("GT911 gefunden auf Adresse 0x%02X\n", gtAddr);
  lcd.setTouchAddr(gtAddr);

  // LCD_CAM/GDMA-Peripherie hart zuruecksetzen, bevor Bus_RGB sie beansprucht.
  Serial.println("Reset LCD_CAM/GDMA-Peripherie ...");
  periph_module_reset(PERIPH_LCD_CAM_MODULE);
  periph_module_reset(PERIPH_GDMA_MODULE);

  Serial.println("lcd.init() ...");
  bool initOk = lcd.init();
  Serial.printf("lcd.init() -> %s\n", initOk ? "OK" : "FEHLGESCHLAGEN");
  if (!initOk) {
    Serial.println("Zweiter Versuch nach 300ms Pause ...");
    delay(300);
    initOk = lcd.init();
    Serial.printf("lcd.init() (2. Versuch) -> %s\n", initOk ? "OK" : "FEHLGESCHLAGEN");
  }

  Serial.println("lcd.initDMA() ...");
  lcd.initDMA();
  Serial.println("lcd.startWrite() ...");
  lcd.startWrite();
  lcd.setRotation(0);

  btnSprite.setColorDepth(16);
  btnSprite.createSprite(220, 60); // groesste Button-Breite/-Hoehe in diesem Sketch

  // Reihenfolge wichtig: Backlight/Buzzer-Kommandos erst NACH lcd.init(),
  // sonst ueberschreibt die Panel-Bring-up-Sequenz den Zustand.
  setBacklightPercent(backlightPercent);

  drawStaticUI();
  Serial.println("Setup fertig.");
}

uint32_t lastHeartbeatMs = 0;

void loop() {
  if (millis() - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = millis();
    Serial.printf("Heartbeat: laeuft seit %lus, freier Heap %u Bytes\n",
                  (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap());
  }

  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    Serial.printf("Touch: x=%d y=%d\n", x, y);

    if (inside(btnBeep, x, y)) {
      drawButton(btnBeep, TFT_GREEN);
      buzzerBeep(150);
      drawButton(btnBeep, TFT_DARKGREY);
    } else if (inside(btnBrightUp, x, y)) {
      backlightPercent = (backlightPercent <= 90) ? backlightPercent + 10 : 100;
      setBacklightPercent(backlightPercent);
      Serial.printf("Backlight: %d%%\n", backlightPercent);
      drawButton(btnBrightUp, TFT_GREEN);
      delay(80);
      drawButton(btnBrightUp, TFT_DARKGREY);
    } else if (inside(btnBrightDn, x, y)) {
      backlightPercent = (backlightPercent >= 10) ? backlightPercent - 10 : 0;
      setBacklightPercent(backlightPercent);
      Serial.printf("Backlight: %d%%\n", backlightPercent);
      drawButton(btnBrightDn, TFT_GREEN);
      delay(80);
      drawButton(btnBrightDn, TFT_DARKGREY);
    } else if (y < 380) {
      bgIndex = (bgIndex + 1) % (sizeof(bgColors) / sizeof(bgColors[0]));
      drawStaticUI();
    }

    delay(150); // einfaches Debounce
  }
}
