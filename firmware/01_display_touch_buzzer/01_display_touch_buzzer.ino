// Stage 1 -- Display + Touch (GT911) + Buzzer/Backlight Funktionstest
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// Arduino-IDE-Einstellungen: siehe firmware/README.md
// Benoetigte Libraries: LovyanGFX 1.2.26 (Library Manager)
//
// Test deckt ab:
//  - RGB-Panel-Bringup ueber LovyanGFX (Bus_RGB/Panel_RGB)
//  - GT911-Touch mit Boot-zu-Boot-Adresswechsel-Fix (0x5D/0x14 Sondierung)
//  - Backlight-Helligkeit und Buzzer ueber STC8H1K28 (I2C 0x30)
//
// Bedienung: Antippen wechselt die Hintergrundfarbe. Die drei Buttons unten
// testen Buzzer und Backlight-Stufen.

#include <Wire.h>
#include "pins.h"
#include "LGFX_Driver.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"

LGFX lcd;

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
  lcd.fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
  lcd.drawRoundRect(b.x, b.y, b.w, b.h, 8, TFT_WHITE);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
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
  lcd.println("Stage 1: Display + Touch + Buzzer");
  drawButton(btnBeep, TFT_DARKGREY);
  drawButton(btnBrightUp, TFT_DARKGREY);
  drawButton(btnBrightDn, TFT_DARKGREY);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 1: Display + Touch + Buzzer -- boot");

  // PSRAM-Diagnose: wenn dieser Wert bei 0 oder sehr klein liegt, ist
  // "OPI PSRAM" im Werkzeuge-Menue NICHT aktiv -- der 768-KB-Framebuffer
  // passt dann nicht ins SRAM und das Panel bleibt schwarz. Springt bei
  // Core-Wechseln haeufig auf Default zurueck, siehe firmware/README.md.
  Serial.printf("PSRAM gesamt: %u Bytes, frei: %u Bytes\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  if (ESP.getPsramSize() == 0) {
    Serial.println("WARNUNG: Kein PSRAM erkannt! Werkzeuge -> PSRAM: OPI PSRAM pruefen.");
  }

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);

  uint8_t gtAddr = probeGT911Address();
  Serial.printf("GT911 gefunden auf Adresse 0x%02X\n", gtAddr);
  lcd.setTouchAddr(gtAddr);

  Serial.println("lcd.init() ...");
  bool initOk = lcd.init();
  Serial.printf("lcd.init() -> %s\n", initOk ? "OK" : "FEHLGESCHLAGEN");
  if (!initOk) {
    // Bring-up der RGB-Bus/Panel-Sequenz ist auf ESP32-S3 bekanntermassen
    // manchmal beim allerersten Versuch nach Kaltstart instabil -- ein
    // zweiter Versuch nach kurzer Pause behebt das in vielen Faellen.
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

  // Reihenfolge wichtig: Backlight/Buzzer-Kommandos erst NACH lcd.init(),
  // sonst ueberschreibt die Panel-Bring-up-Sequenz den Zustand.
  setBacklightPercent(backlightPercent);

  drawStaticUI();
  Serial.println("Setup fertig.");
}

uint32_t lastHeartbeatMs = 0;

void loop() {
  // Heartbeat: laeuft auch dann weiter, wenn das Panel schwarz bleibt --
  // zeigt, ob der Kern haengt oder nur die RGB-Ausgabe fehlt.
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
      // Tap auf freie Flaeche -> Hintergrundfarbe wechseln (Panel-Refresh-Test)
      bgIndex = (bgIndex + 1) % (sizeof(bgColors) / sizeof(bgColors[0]));
      drawStaticUI();
    }

    delay(150); // einfaches Debounce
  }
}
