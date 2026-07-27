#pragma once
#include <Wire.h>
#include "pins.h"

// PCF8563 Echtzeituhr, Adresse 0x51. Zeitregister ab 0x02 (NICHT 0x04 wie
// beim aehnlich benannten PCF85063 -- Verwechslungsgefahr, siehe README.md
// Abschnitt 6). Direkter Wire-Zugriff, keine externe Library noetig.
// Pufferung ueber CR1220-Knopfzelle auf dem Board.

struct RtcDateTime {
  uint16_t year;   // z. B. 2026
  uint8_t  month;  // 1-12
  uint8_t  day;    // 1-31
  uint8_t  weekday; // 0=Sonntag .. 6=Samstag
  uint8_t  hour;   // 0-23
  uint8_t  minute; // 0-59
  uint8_t  second; // 0-59
  bool     voltageLow; // true = VL-Bit gesetzt, Zeit ggf. unzuverlaessig
};

inline uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
inline uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

inline bool rtcRead(RtcDateTime &out) {
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom((int)PCF8563_ADDR, 7) != 7) return false;

  uint8_t regSeconds = Wire.read();
  uint8_t regMinutes = Wire.read();
  uint8_t regHours   = Wire.read();
  uint8_t regDays    = Wire.read();
  uint8_t regWeekday = Wire.read();
  uint8_t regMonths  = Wire.read();
  uint8_t regYears   = Wire.read();

  out.voltageLow = regSeconds & 0x80;
  out.second  = bcdToDec(regSeconds & 0x7F);
  out.minute  = bcdToDec(regMinutes & 0x7F);
  out.hour    = bcdToDec(regHours & 0x3F);
  out.day     = bcdToDec(regDays & 0x3F);
  out.weekday = regWeekday & 0x07;
  out.month   = bcdToDec(regMonths & 0x1F);
  out.year    = 2000 + bcdToDec(regYears); // Century-Bit (regMonths Bit7) wird hier ignoriert (2000er Jahre)

  return true;
}

inline bool rtcWrite(const RtcDateTime &dt) {
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(0x02);
  Wire.write(decToBcd(dt.second) & 0x7F);   // VL-Bit beim Schreiben loeschen
  Wire.write(decToBcd(dt.minute) & 0x7F);
  Wire.write(decToBcd(dt.hour) & 0x3F);
  Wire.write(decToBcd(dt.day) & 0x3F);
  Wire.write(dt.weekday & 0x07);
  Wire.write(decToBcd(dt.month) & 0x1F);
  Wire.write(decToBcd(dt.year - 2000));
  return Wire.endTransmission() == 0;
}

inline void rtcFormat(const RtcDateTime &dt, char *buf, size_t bufLen) {
  snprintf(buf, bufLen, "%04u-%02u-%02u %02u:%02u:%02u%s",
           dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
           dt.voltageLow ? "  (VL! Zeit unsicher)" : "");
}
