#pragma once
// RGB-Panel-Bringup ueber Espressifs eigenen ESP-IDF-Treiber (esp_lcd_panel_rgb),
// NICHT ueber LovyanGFX's Bus_RGB/Panel_RGB.
//
// Grund fuer esp_lcd_panel_rgb allgemein: LovyanGFX's Bus_RGB hat keinen
// Anti-Tearing-Mechanismus (siehe src/lgfx/v1/platforms/esp32s3/Bus_RGB.hpp --
// config_t enthaelt nur reine Pin-/Timing-Parameter). Der ESP32-S3 hat ein
// bekanntes Problem: liegt der Framebuffer im PSRAM (zwingend, da SRAM zu
// klein ist) und schreibt die CPU gleichzeitig hinein, waehrend die GDMA-
// Hardware denselben Speicher kontinuierlich fuer die Bildausgabe ausliest,
// entstehen sichtbare Bildfehler.
//
// esp_lcd_panel_rgb bietet dafuer zwei Anti-Tearing-Strategien, laut
// ESP-IDF NICHT gleichzeitig nutzbar:
//  1. Bounce Buffer (fruehere Version dieser Datei): ein kleiner SRAM-
//     Zwischenpuffer, den die GDMA ausliest, waehrend er im Hintergrund aus
//     dem PSRAM-Framebuffer nachgefuellt wird. Loest reine Bandbreiten-
//     Streifen -- aber NICHT das Problem, dass ein direkt in den einzigen
//     Framebuffer schreibender Teil-Redraw waehrend des Auslesens sichtbare
//     "Geister" alter Zeichenoperationen hinterlassen kann (beobachtet in
//     Stage 2 bei den Text-Updates: schwache Text-Reste über den echten
//     Zeilen).
//  2. Doppelter Framebuffer (num_fbs = 2, jetzt aktiv): die CPU zeichnet nie
//     in den Framebuffer, der gerade von der GDMA ausgelesen wird --
//     esp_lcd_panel_draw_bitmap() kopiert fertige Bilddaten in den jeweils
//     INAKTIVEN Framebuffer und tauscht ihn erst beim naechsten VSYNC
//     sichtbar um. Tearing-frei, kostet aber den doppelten Framebuffer-
//     Speicher (bei 800x480x16bpp: 2 x 750 KiB PSRAM -- bei 8 MiB PSRAM kein
//     Problem).
//
// Konsequenz fuer die *.ino/main.cpp-Dateien: "canvas" zeigt NICHT mehr
// direkt auf einen der beiden Hardware-Framebuffer, sondern auf einen
// dritten, eigenen PSRAM-Puffer, den die GDMA nie sieht. Darin darf beliebig
// oft und beliebig granular (Teil-Redraws) gezeichnet werden, ohne jemals
// Tearing zu riskieren. Erst wenn eine zusammengehoerige Aenderung fertig
// ist, wird ihr kompletter Inhalt per rgbPanelFlush() in den Hardware-
// Doppelpuffer kopiert.

#include <Arduino.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "pins.h"

static esp_lcd_panel_handle_t g_rgbPanelHandle = nullptr;

inline bool rgbPanelInit() {
  esp_lcd_rgb_panel_config_t panel_config = {};
  panel_config.clk_src = LCD_CLK_SRC_DEFAULT;

  panel_config.timings.pclk_hz = LCD_FREQ_WRITE;
  panel_config.timings.h_res = LCD_WIDTH;
  panel_config.timings.v_res = LCD_HEIGHT;
  panel_config.timings.hsync_pulse_width = 4;
  panel_config.timings.hsync_back_porch = 8;
  panel_config.timings.hsync_front_porch = 8;
  panel_config.timings.vsync_pulse_width = 4;
  panel_config.timings.vsync_back_porch = 8;
  panel_config.timings.vsync_front_porch = 8;
  // Polaritaets-Flags: 1:1 aus den bisherigen LovyanGFX-Bus_RGB-Werten
  // uebernommen (hsync_polarity=0, vsync_polarity=0, pclk_idle_high=1,
  // pclk_active_neg=Standardwert 1 in LovyanGFX). Falls das Bild nach dem
  // Umbau verschoben/zerrissen/falsch gefaerbt ist: zuerst hier
  // pclk_active_neg und *_idle_low/de_idle_high durchprobieren.
  panel_config.timings.flags.hsync_idle_low = 0;
  panel_config.timings.flags.vsync_idle_low = 0;
  panel_config.timings.flags.de_idle_high = 0;
  panel_config.timings.flags.pclk_active_neg = 1;
  panel_config.timings.flags.pclk_idle_high = 1;

  panel_config.data_width = 16;
  panel_config.bits_per_pixel = 16;
  // Doppelter Framebuffer statt Bounce Buffer -- siehe Erklaerung oben.
  panel_config.num_fbs = 2;

  panel_config.hsync_gpio_num = PIN_LCD_HSYNC;
  panel_config.vsync_gpio_num = PIN_LCD_VSYNC;
  panel_config.de_gpio_num = PIN_LCD_DE;
  panel_config.pclk_gpio_num = PIN_LCD_PCLK;
  panel_config.disp_gpio_num = -1;

  // Reihenfolge wie im RGB565-Standard und wie bisher in LovyanGFX
  // (Bus_RGB d0..d15): Bit0-4=Blau, Bit5-10=Gruen, Bit11-15=Rot.
  panel_config.data_gpio_nums[0]  = PIN_LCD_B0;
  panel_config.data_gpio_nums[1]  = PIN_LCD_B1;
  panel_config.data_gpio_nums[2]  = PIN_LCD_B2;
  panel_config.data_gpio_nums[3]  = PIN_LCD_B3;
  panel_config.data_gpio_nums[4]  = PIN_LCD_B4;
  panel_config.data_gpio_nums[5]  = PIN_LCD_G0;
  panel_config.data_gpio_nums[6]  = PIN_LCD_G1;
  panel_config.data_gpio_nums[7]  = PIN_LCD_G2;
  panel_config.data_gpio_nums[8]  = PIN_LCD_G3;
  panel_config.data_gpio_nums[9]  = PIN_LCD_G4;
  panel_config.data_gpio_nums[10] = PIN_LCD_G5;
  panel_config.data_gpio_nums[11] = PIN_LCD_R0;
  panel_config.data_gpio_nums[12] = PIN_LCD_R1;
  panel_config.data_gpio_nums[13] = PIN_LCD_R2;
  panel_config.data_gpio_nums[14] = PIN_LCD_R3;
  panel_config.data_gpio_nums[15] = PIN_LCD_R4;

  panel_config.flags.fb_in_psram = 1;

  esp_err_t err = esp_lcd_new_rgb_panel(&panel_config, &g_rgbPanelHandle);
  if (err != ESP_OK) {
    Serial.printf("esp_lcd_new_rgb_panel() fehlgeschlagen: %d\n", (int)err);
    return false;
  }

  err = esp_lcd_panel_reset(g_rgbPanelHandle);
  if (err != ESP_OK) {
    Serial.printf("esp_lcd_panel_reset() fehlgeschlagen: %d\n", (int)err);
    return false;
  }

  err = esp_lcd_panel_init(g_rgbPanelHandle);
  if (err != ESP_OK) {
    Serial.printf("esp_lcd_panel_init() fehlgeschlagen: %d\n", (int)err);
    return false;
  }

  return true;
}

// Kopiert den kompletten Inhalt von buf (muss LCD_WIDTH x LCD_HEIGHT Pixel
// im 16bpp-Format des canvas-Sprites sein) in den aktuell inaktiven
// Hardware-Framebuffer und macht ihn erst beim naechsten VSYNC sichtbar --
// tearing-frei dank num_fbs = 2 oben. Nach jeder zusammengehoerigen
// Zeichenoperation auf "canvas" einmal aufrufen (nicht nach jedem einzelnen
// fillRect/drawString -- das waere unnoetig teuer).
inline void rgbPanelFlush(void *buf) {
  esp_lcd_panel_draw_bitmap(g_rgbPanelHandle, 0, 0, LCD_WIDTH, LCD_HEIGHT, buf);
}
