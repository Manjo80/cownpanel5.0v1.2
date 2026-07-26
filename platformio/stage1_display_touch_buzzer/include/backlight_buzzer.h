#pragma once
#include <Wire.h>
#include "pins.h"

// STC8H1K28-Controller (I2C-Adresse 0x30) fuer Backlight, Buzzer und Speaker.
// Ein einzelnes Kommando-Byte pro Aufruf, kein Register-Adressbyte.
//
// WICHTIG: Erst NACH lcd.init() aufrufen -- die Panel-Initialisierungssequenz
// ueberschreibt sonst den gesendeten Zustand (siehe README.md Abschnitt 8).
inline void sendBacklightBuzzerCmd(uint8_t cmd) {
  Wire.beginTransmission(BACKLIGHT_BUZZER_ADDR);
  Wire.write(cmd);
  Wire.endTransmission();
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
