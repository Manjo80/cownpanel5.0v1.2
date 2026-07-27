#pragma once
// GT911-Touch eigenstaendig ueber LovyanGFX's Touch_GT911-Klasse, OHNE dass
// sie an ein LGFX_Device/Panel angehaengt ist (wir nutzen kein Panel_RGB
// mehr, siehe rgb_panel.h). Touch_GT911 ist im Kern nur ein I2C-Treiber und
// funktioniert genauso gut standalone -- lediglich die komfortable
// rotations-/kalibrierungsbewusste lcd.getTouch(x,y)-Huelle aus LGFXBase
// entfaellt, die wir hier von Hand nachbilden (bei offset_rotation=0 und
// x/y-Min/Max = Panelaufloesung ist das eine reine Identitaetsabbildung).

#include <LovyanGFX.hpp>
#include "pins.h"

static lgfx::Touch_GT911 g_touch;

inline void touchStandaloneConfig(uint8_t i2cAddr) {
  auto cfg = g_touch.config();
  cfg.x_min = 0;
  cfg.x_max = LCD_WIDTH;
  cfg.y_min = 0;
  cfg.y_max = LCD_HEIGHT;
  cfg.pin_int = -1;
  cfg.bus_shared = false; // 1:1 wie Elecrows Werks-Testcode
  cfg.offset_rotation = 0;
  cfg.i2c_port = I2C_NUM_0;
  cfg.pin_sda = PIN_I2C_SDA;
  cfg.pin_scl = PIN_I2C_SCL;
  cfg.pin_rst = -1;
  cfg.freq = I2C_FREQ_HZ;
  cfg.i2c_addr = i2cAddr;
  g_touch.config(cfg);
}

inline bool touchStandaloneInit() {
  return g_touch.init();
}

// Ersatz fuer das bisherige lcd.getTouch(&x, &y).
inline bool touchStandaloneGetXY(int32_t *x, int32_t *y) {
  lgfx::touch_point_t tp;
  uint_fast8_t count = g_touch.getTouchRaw(&tp, 1);
  if (count == 0) return false;
  *x = tp.x;
  *y = tp.y;
  return true;
}
