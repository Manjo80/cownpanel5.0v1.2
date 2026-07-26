// Stage 1 -- Display + Touch (GT911) + Buzzer/Backlight Funktionstest
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// Arduino-IDE-Einstellungen: siehe firmware/README.md
// Benoetigte Libraries: LovyanGFX 1.2.26 (Library Manager) -- nur noch fuer
// Zeichen-API (Sprites/Fonts/Formen) genutzt, NICHT mehr fuer den RGB-Bus.
//
// GROSSER UMBAU: Die RGB-Panel-Hardware-Ausgabe laeuft jetzt ueber
// Espressifs eigenen ESP-IDF-Treiber (esp_lcd_panel_rgb, siehe
// rgb_panel.h) statt ueber LovyanGFX's Bus_RGB/Panel_RGB. Grund: nur
// Espressifs Treiber hat einen "Bounce Buffer" gegen PSRAM-Bandbreiten-
// Konkurrenz zwischen CPU-Schreibzugriffen und der kontinuierlichen
// GDMA-Bildausgabe -- das war der wahrscheinliche Grund fuer die
// hartnaeckigen Streifen/Bildfehler, die mit reinen LovyanGFX-Anpassungen
// (Sprite-Puffer, waitDMA, PSRAM-Takt, Pixeltakt) nicht behoben werden
// konnten. Details siehe rgb_panel.h und touch_standalone.h.
//
// Test deckt ab:
//  - RGB-Panel-Bringup ueber esp_lcd_panel_rgb mit Bounce Buffer
//  - GT911-Touch mit deterministischer Power-On-Sequenz (touch_probe.h)
//  - Backlight-Helligkeit und Buzzer ueber STC8H1K28 (I2C 0x30)
//
// Bedienung: Antippen wechselt die Hintergrundfarbe. Die drei Buttons unten
// testen Buzzer und Backlight-Stufen.

#include <Wire.h>
#include <esp_system.h>
#include "sdkconfig.h"
#include "pins.h"
#include "rgb_panel.h"
#include "touch_standalone.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"

// Sprite ohne eigenen Speicher -- zeigt per setBuffer() direkt auf den
// von rgbPanelInit() bereitgestellten PSRAM-Framebuffer. Alle Zeichen-
// Aufrufe (fillScreen, drawString, ...) schreiben damit direkt in den
// echten, von esp_lcd_panel_rgb verwalteten Framebuffer.
LGFX_Sprite canvas;

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

void drawStaticUI() {
  canvas.fillScreen(bgColors[bgIndex]);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(3);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setCursor(20, 20);
  canvas.println("CrowPanel Advance 5.0");
  canvas.setTextSize(2);
  canvas.println("Stage 1: Display + Touch + Buzzer (esp_lcd_panel_rgb)");
  drawButton(btnBeep, TFT_DARKGREY);
  drawButton(btnBrightUp, TFT_DARKGREY);
  drawButton(btnBrightDn, TFT_DARKGREY);
}

void setup() {
  // Muss vor allem anderen laufen (siehe touch_probe.h) -- zwingt den GT911
  // deterministisch auf Adresse 0x5D, statt den Pegel floaten zu lassen.
  gt911PowerOnSequence();

  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 1: Display + Touch + Buzzer -- boot");

  // Objektiver Reset-Grund statt "Reset-Knopf gedrueckt" nach Gefuehl --
  // zeigt z. B. POWERON_RESET (Stromlos) vs. SW_CPU_RESET/USB_UART_CHIP_RESET
  // (Reset-Knopf/Serial-Monitor-Autoreset).
  Serial.printf("Reset-Grund: %d\n", (int)esp_reset_reason());

  // PSRAM-Takt, mit dem der GERADE INSTALLIERTE Core kompiliert wurde.
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
    Serial.println("WARNUNG: Kein PSRAM erkannt! Werkzeuge -> PSRAM: OPI PSRAM pruefen.");
  }

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);

  // Zusaetzlich zur 120ms-Adress-Auswahl-Sequenz (gt911PowerOnSequence())
  // braucht der GT911-Chip selbst nach einem ECHTEN Stromlos-Start noch
  // Zeit fuer sein eigenes Power-On-Booting, bevor er auf dem I2C-Bus
  // ueberhaupt antwortet. Nach einem Reset-Knopf-Druck laeuft der Chip
  // bereits, daher faellt das dort nicht auf. Ohne diese Wartezeit bleibt
  // Touch nach echtem Stromlos-Start tot, obwohl dieselbe Sequenz nach
  // Reset-Knopf zuverlaessig funktioniert.
  const uint32_t GT911_READY_DELAY_MS = 600;
  Serial.printf("Warte %lums auf GT911-Boot (nur bei echtem Stromlos-Start noetig) ...\n",
                (unsigned long)GT911_READY_DELAY_MS);
  delay(GT911_READY_DELAY_MS);

  uint8_t gtAddr = probeGT911Address();
  Serial.printf("GT911 gefunden auf Adresse 0x%02X\n", gtAddr);
  touchStandaloneConfig(gtAddr);
  Serial.printf("Touch-Init -> %s\n", touchStandaloneInit() ? "OK" : "FEHLGESCHLAGEN");

  Serial.println("rgbPanelInit() (esp_lcd_panel_rgb, mit Bounce Buffer) ...");
  bool panelOk = rgbPanelInit();
  Serial.printf("rgbPanelInit() -> %s\n", panelOk ? "OK" : "FEHLGESCHLAGEN");
  if (!panelOk) {
    Serial.println("Panel-Init fehlgeschlagen -- Sketch haelt an.");
    while (true) { delay(1000); }
  }

  canvas.setColorDepth(16);
  canvas.setBuffer(g_rgbFrameBuffer, LCD_WIDTH, LCD_HEIGHT, 16);

  // Reihenfolge wichtig: Backlight/Buzzer-Kommandos erst NACH dem Panel-Bringup,
  // sonst ueberschreibt die Panel-Initialisierungssequenz den Zustand.
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
  if (touchStandaloneGetXY(&x, &y)) {
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
