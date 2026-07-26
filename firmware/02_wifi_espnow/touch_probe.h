#pragma once
#include <Wire.h>
#include "pins.h"

// Der GT911 waehlt seine I2C-Adresse (0x5D oder 0x14) beim Power-On anhand
// des Pegels an seinem INT-Pin. Da dieser Pin vom ESP32 nicht angesteuert
// wird (cfg.pin_int = -1), floatet der Pegel und die Adresse kann zwischen
// Boots wechseln (siehe README.md Abschnitt 9). Deshalb IMMER zur Laufzeit
// sondieren, statt die Adresse fest zu verdrahten.
//
// Muss aufgerufen werden NACHDEM Wire.begin() lief, aber BEVOR lcd.init()
// aufgerufen wird.
inline uint8_t probeGT911Address() {
  Wire.beginTransmission(GT911_ADDR_PRIMARY);
  if (Wire.endTransmission() == 0) return GT911_ADDR_PRIMARY;

  Wire.beginTransmission(GT911_ADDR_SECONDARY);
  if (Wire.endTransmission() == 0) return GT911_ADDR_SECONDARY;

  // Chip antwortet auf keiner der beiden Adressen -- Fallback auf primaere
  // Adresse, Touch wird dann vermutlich nicht funktionieren (Verkabelung/
  // Power pruefen).
  return GT911_ADDR_PRIMARY;
}
