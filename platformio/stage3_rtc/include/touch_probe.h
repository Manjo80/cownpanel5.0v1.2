#pragma once
#include <Wire.h>
#include "pins.h"

// Aus Elecrows Werks-Testcode uebernommen ("GT911 上电时序 ---> 选用 0x5D" --
// GT911-Power-On-Sequenz, waehlt 0x5D): GPIO1 ist am INT-Pin des GT911
// angeschlossen. Der GT911 entscheidet beim eigenen Power-On-Reset anhand
// des Pegels an diesem Pin, ob er sich auf Adresse 0x5D oder 0x14 legt.
// Indem der ESP32 GPIO1 selbst fuer 120ms aktiv auf LOW zieht (statt es
// floaten zu lassen) und danach wieder auf Eingang stellt, wird die Adresse
// deterministisch auf 0x5D erzwungen, statt sie dem Zufall zu ueberlassen.
//
// MUSS so frueh wie moeglich in setup() laufen -- noch VOR Wire.begin() --
// damit der Pegel bereits feststeht, wenn der GT911 seine eigene
// Power-On-Reset-Sequenz abschliesst (die parallel zum ESP32-Boot laeuft).
inline void gt911PowerOnSequence() {
  pinMode(PIN_GT911_INT, OUTPUT);
  digitalWrite(PIN_GT911_INT, LOW);
  delay(120);
  pinMode(PIN_GT911_INT, INPUT);
}

// Sicherheitsnetz: sondiert zur Laufzeit, ob der GT911 trotz obiger Sequenz
// auf einer anderen Adresse antwortet (z. B. bei abweichender Hardware-
// Revision). Muss aufgerufen werden NACHDEM Wire.begin() lief, aber BEVOR
// lcd.init() aufgerufen wird.
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
