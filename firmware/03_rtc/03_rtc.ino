// Stage 3 -- RTC (PCF8563) Funktionstest
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// Zeigt die laufende Uhrzeit der PCF8563-RTC an. Zwei Touch-Buttons:
//  - "STELLEN (Compile-Zeit)": schreibt Sketch-Kompilierzeit in die RTC,
//    funktioniert offline, ohne WLAN.
//  - "NTP-SYNC": optional, nur wenn USE_WIFI_NTP_SYNC aktiviert UND
//    wifi_secrets.h vorhanden ist (siehe wifi_secrets.example.h in Stage 2).

#include <Wire.h>
#include <stdio.h>
#include <string.h>
#include "pins.h"
#include "LGFX_Driver.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"
#include "rtc_pcf8563.h"

// Fuer echten NTP-Abgleich einkommentieren und wifi_secrets.h anlegen
// (Kopie von firmware/02_wifi_espnow/wifi_secrets.example.h in diesen Ordner).
// #define USE_WIFI_NTP_SYNC
#ifdef USE_WIFI_NTP_SYNC
#include <WiFi.h>
#include <time.h>
#include "wifi_secrets.h"
#endif

LGFX lcd;

struct Button { int x, y, w, h; const char *label; };
Button btnSetCompile = { 40,  400, 340, 60, "STELLEN (Compile-Zeit)" };
Button btnNtpSync     = { 420, 400, 340, 60, "NTP-SYNC" };

bool inside(const Button &b, int32_t x, int32_t y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

void drawButton(const Button &b, uint16_t fill) {
  lcd.fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
  lcd.drawRoundRect(b.x, b.y, b.w, b.h, 8, TFT_WHITE);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextSize(2);
  lcd.setTextDatum(lgfx::middle_center);
  lcd.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
}

// Sagt anhand von Julian-Day-artiger Formel den Wochentag voraus (0=So..6=Sa)
uint8_t computeWeekday(uint16_t y, uint8_t m, uint8_t d) {
  static const uint8_t t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y -= 1;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// Parst __DATE__ ("Mmm dd yyyy") und __TIME__ ("hh:mm:ss") und schreibt in die RTC.
void setRtcFromCompileTime() {
  const char *monthNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monStr[4];
  int day, year, hour, minute, second;
  sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
  int month = (strstr(monthNames, monStr) - monthNames) / 3 + 1;

  RtcDateTime dt;
  dt.year = year; dt.month = month; dt.day = day;
  dt.hour = hour; dt.minute = minute; dt.second = second;
  dt.weekday = computeWeekday(year, month, day);
  dt.voltageLow = false;

  if (rtcWrite(dt)) {
    Serial.println("RTC auf Compile-Zeit gestellt.");
  } else {
    Serial.println("FEHLER: RTC-Schreibzugriff fehlgeschlagen (Verkabelung/Adresse pruefen).");
  }
}

#ifdef USE_WIFI_NTP_SYNC
void syncFromNtp() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Verbinde mit WLAN fuer NTP");
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 10000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WLAN-Verbindung fehlgeschlagen, NTP-Sync abgebrochen.");
    return;
  }

  // EU-Zeitzonenregel (Luxemburg) inkl. automatischer Sommerzeitumstellung.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "europe.pool.ntp.org");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP-Sync fehlgeschlagen (Timeout).");
    return;
  }

  RtcDateTime dt;
  dt.year = timeinfo.tm_year + 1900;
  dt.month = timeinfo.tm_mon + 1;
  dt.day = timeinfo.tm_mday;
  dt.hour = timeinfo.tm_hour;
  dt.minute = timeinfo.tm_min;
  dt.second = timeinfo.tm_sec;
  dt.weekday = timeinfo.tm_wday;
  dt.voltageLow = false;

  if (rtcWrite(dt)) {
    Serial.println("RTC per NTP synchronisiert.");
  } else {
    Serial.println("FEHLER: RTC-Schreibzugriff nach NTP-Sync fehlgeschlagen.");
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Stage 3: RTC (PCF8563) -- boot");

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
  lcd.println("Stage 3: RTC (PCF8563)");
  drawButton(btnSetCompile, TFT_DARKGREY);
  drawButton(btnNtpSync, TFT_DARKGREY);

  RtcDateTime dt;
  if (rtcRead(dt) && dt.voltageLow) {
    Serial.println("Hinweis: VL-Bit gesetzt -- Zeit nach Spannungsausfall unsicher, bitte stellen.");
  }
}

uint32_t lastDrawMs = 0;

void loop() {
  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    if (inside(btnSetCompile, x, y)) {
      drawButton(btnSetCompile, TFT_GREEN);
      setRtcFromCompileTime();
      buzzerBeep(80);
      delay(200);
      drawButton(btnSetCompile, TFT_DARKGREY);
    } else if (inside(btnNtpSync, x, y)) {
      drawButton(btnNtpSync, TFT_GREEN);
#ifdef USE_WIFI_NTP_SYNC
      syncFromNtp();
#else
      Serial.println("NTP-Sync deaktiviert -- USE_WIFI_NTP_SYNC einkommentieren und wifi_secrets.h anlegen.");
#endif
      buzzerBeep(80);
      delay(200);
      drawButton(btnNtpSync, TFT_DARKGREY);
    }
  }

  if (millis() - lastDrawMs >= 1000) {
    lastDrawMs = millis();
    RtcDateTime dt;
    char buf[64];
    lcd.fillRect(20, 100, 760, 60, TFT_BLACK);
    lcd.setTextSize(3);
    lcd.setCursor(20, 100);
    if (rtcRead(dt)) {
      rtcFormat(dt, buf, sizeof(buf));
      lcd.setTextColor(dt.voltageLow ? TFT_RED : TFT_GREENYELLOW);
      lcd.print(buf);
    } else {
      lcd.setTextColor(TFT_RED);
      lcd.print("RTC-Lesefehler (I2C pruefen)");
    }
  }
}
