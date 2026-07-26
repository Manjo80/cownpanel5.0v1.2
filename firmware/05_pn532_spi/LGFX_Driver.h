#pragma once
// LovyanGFX-Konfiguration fuer CrowPanel Advance 5.0 (RGB-Panel + GT911-Touch).
// Benoetigt LovyanGFX 1.2.26 (NUR diese Version im libraries-Ordner, siehe
// README.md Abschnitt 1 des Hauptrepos -- aeltere 1.1.x kompiliert auf
// ESP32-Arduino-Core 3.x nicht).

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2c.h>
#include "pins.h"

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width  = LCD_WIDTH;
      cfg.memory_height = LCD_HEIGHT;
      cfg.panel_width   = LCD_WIDTH;
      cfg.panel_height  = LCD_HEIGHT;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _panel_instance.config_detail();
      cfg.use_psram = 1;   // zwingend: Framebuffer 800x480x2 Byte = 768 KB
      _panel_instance.config_detail(cfg);
    }
    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      cfg.pin_d0  = (gpio_num_t)PIN_LCD_B0;
      cfg.pin_d1  = (gpio_num_t)PIN_LCD_B1;
      cfg.pin_d2  = (gpio_num_t)PIN_LCD_B2;
      cfg.pin_d3  = (gpio_num_t)PIN_LCD_B3;
      cfg.pin_d4  = (gpio_num_t)PIN_LCD_B4;
      cfg.pin_d5  = (gpio_num_t)PIN_LCD_G0;
      cfg.pin_d6  = (gpio_num_t)PIN_LCD_G1;
      cfg.pin_d7  = (gpio_num_t)PIN_LCD_G2;
      cfg.pin_d8  = (gpio_num_t)PIN_LCD_G3;
      cfg.pin_d9  = (gpio_num_t)PIN_LCD_G4;
      cfg.pin_d10 = (gpio_num_t)PIN_LCD_G5;
      cfg.pin_d11 = (gpio_num_t)PIN_LCD_R0;
      cfg.pin_d12 = (gpio_num_t)PIN_LCD_R1;
      cfg.pin_d13 = (gpio_num_t)PIN_LCD_R2;
      cfg.pin_d14 = (gpio_num_t)PIN_LCD_R3;
      cfg.pin_d15 = (gpio_num_t)PIN_LCD_R4;

      cfg.pin_henable = (gpio_num_t)PIN_LCD_DE;
      cfg.pin_vsync   = (gpio_num_t)PIN_LCD_VSYNC;
      cfg.pin_hsync   = (gpio_num_t)PIN_LCD_HSYNC;
      cfg.pin_pclk    = (gpio_num_t)PIN_LCD_PCLK;
      cfg.freq_write  = LCD_FREQ_WRITE;

      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch  = 8;
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch  = 8;
      cfg.pclk_idle_high    = 1;

      _bus_instance.config(cfg);
    }
    _panel_instance.setBus(&_bus_instance);

    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = LCD_WIDTH;
      cfg.y_min = 0;
      cfg.y_max = LCD_HEIGHT;
      cfg.pin_int = -1;      // Interrupt-Pin nicht angesteuert
      cfg.bus_shared = true; // Touch teilt den I2C-Bus mit RTC/Backlight-IC
      cfg.offset_rotation = 0;
      cfg.i2c_port = I2C_NUM_0;
      cfg.pin_sda = PIN_I2C_SDA;
      cfg.pin_scl = PIN_I2C_SCL;
      cfg.pin_rst = -1;
      cfg.freq = I2C_FREQ_HZ;
      cfg.i2c_addr = GT911_ADDR_PRIMARY;  // vor lcd.init() ggf. per setTouchAddr() ueberschreiben
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }

  // Muss VOR init() aufgerufen werden, falls probeGT911Address() (siehe
  // touch_probe.h) die sekundaere Adresse 0x14 ermittelt hat.
  void setTouchAddr(uint8_t addr) {
    auto cfg = _touch_instance.config();
    cfg.i2c_addr = addr;
    _touch_instance.config(cfg);
  }
};
