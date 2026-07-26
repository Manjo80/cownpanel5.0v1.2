#pragma once
// RGB-Panel-Bringup ueber Espressifs eigenen ESP-IDF-Treiber (esp_lcd_panel_rgb),
// NICHT ueber LovyanGFX's Bus_RGB/Panel_RGB.
//
// Grund: LovyanGFX's Bus_RGB hat keinen "Bounce Buffer" (siehe
// src/lgfx/v1/platforms/esp32s3/Bus_RGB.hpp -- config_t enthaelt nur reine
// Pin-/Timing-Parameter, keine Bounce-Buffer-Option). Der ESP32-S3 hat ein
// bekanntes Problem: liegt der Framebuffer im PSRAM (zwingend, da SRAM zu
// klein ist) und schreibt die CPU gleichzeitig hinein (z. B. beim Zeichnen
// eines Buttons), konkurriert das mit der GDMA-Hardware, die denselben
// Speicher kontinuierlich fuer die Bildausgabe ausliest -- beide teilen
// sich dieselbe PSRAM-Bandbreite. Espressifs eigener Treiber hat genau
// dafuer einen Bounce Buffer eingebaut: ein kleiner Zwischenpuffer im
// schnellen internen SRAM, den die GDMA ausliest, waehrend er im
// Hintergrund kontrolliert aus dem PSRAM-Framebuffer nachgefuellt wird --
// das entkoppelt die zeitkritische Bildausgabe von CPU-Schreibzugriffen.
//
// Wir nutzen weiterhin LovyanGFX's Zeichen-API (Sprites, Fonts, Formen),
// aber nur noch als "Sprite", der DIREKT auf den von diesem Treiber
// verwalteten Framebuffer zeigt (LGFX_Sprite::setBuffer()) -- die
// tatsaechliche Hardware-Ausgabe (Timing, DMA, Bounce Buffer) macht
// vollstaendig esp_lcd_panel_rgb, nicht mehr LovyanGFX.

#include <Arduino.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "pins.h"

static esp_lcd_panel_handle_t g_rgbPanelHandle = nullptr;
static void *g_rgbFrameBuffer = nullptr;

// bounce_buffer_size_px: Groesse des SRAM-Zwischenpuffers in Pixeln, in
// beide Richtungen doppelt vorgehalten. 10 Bildzeilen sind ein in
// Espressifs eigenen Beispielen fuer 800x480-Panels ueblicher Startwert --
// bei Bedarf (weiterhin Bildfehler oder zu hoher SRAM-Verbrauch) anpassen.
static const size_t RGB_BOUNCE_BUFFER_LINES = 10;

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
  panel_config.num_fbs = 1;
  panel_config.bounce_buffer_size_px = LCD_WIDTH * RGB_BOUNCE_BUFFER_LINES;
  panel_config.psram_trans_align = 64;

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

  err = esp_lcd_rgb_panel_get_frame_buffer(g_rgbPanelHandle, 1, &g_rgbFrameBuffer);
  if (err != ESP_OK || g_rgbFrameBuffer == nullptr) {
    Serial.printf("esp_lcd_rgb_panel_get_frame_buffer() fehlgeschlagen: %d\n", (int)err);
    return false;
  }

  return true;
}
