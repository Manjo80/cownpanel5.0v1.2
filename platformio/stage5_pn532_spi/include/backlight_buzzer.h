#pragma once
#include <Wire.h>
#include "pins.h"

// STC8H1K28-Controller (I2C-Adresse 0x30) fuer Backlight, Buzzer und Speaker.
// Ein einzelnes Kommando-Byte pro Aufruf, kein Register-Adressbyte.
//
// WICHTIG: Erst NACH lcd.init() aufrufen -- die Panel-Initialisierungssequenz
// ueberschreibt sonst den gesendeten Zustand (siehe README.md Abschnitt 8).
//
// Direkt nach lcd.init() kann die ALLERERSTE I2C-Uebertragung fehlschlagen
// (beobachtet als "i2cWrite failed" kurz nachdem LovyanGFX/Touch_GT911 den
// I2C-Bus mit bus_shared=true erneut anfasst -- siehe "setPins(): bus
// already initialized"-Warnung). Ohne Wiederholung blieb das Backlight in
// diesem Fall dauerhaft AUS und das Display wirkte "schwarz", obwohl das
// Rendering laengst korrekt lief. Deshalb hier automatisch bis zu 3x
// wiederholen, bevor aufgegeben wird.
inline bool sendBacklightBuzzerCmd(uint8_t cmd) {
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    Wire.beginTransmission(BACKLIGHT_BUZZER_ADDR);
    Wire.write(cmd);
    uint8_t result = Wire.endTransmission();
    if (result == 0) return true;
    Serial.printf("Backlight/Buzzer I2C-Schreibfehler (Code %u), Versuch %u/3 ...\n",
                  result, attempt);
    delay(20);
  }
  Serial.println("Backlight/Buzzer-Kommando endgueltig fehlgeschlagen.");
  return false;
}

// percent: 0 = aus, 1-100 = Helligkeit (100 = maximal hell).
// Chip-intern ist die Skala invertiert (0=hell...244=dunkel, 245=aus).
inline void setBacklightPercent(uint8_t percent) {
  if (percent == 0) {
    sendBacklightBuzzerCmd(CMD_BACKLIGHT_OFF);
    return;
  }
  if (percent > 100) percent = 100;
  uint8_t level = (uint8_t)((100 - percent) * CMD_BACKLIGHT_MIN / 100); // 0..244
  sendBacklightBuzzerCmd(level);
}

inline void buzzerOn()  { sendBacklightBuzzerCmd(CMD_BUZZER_ON); }
inline void buzzerOff() { sendBacklightBuzzerCmd(CMD_BUZZER_OFF); }

inline void buzzerBeep(uint16_t ms = 120) {
  buzzerOn();
  delay(ms);
  buzzerOff();
}
