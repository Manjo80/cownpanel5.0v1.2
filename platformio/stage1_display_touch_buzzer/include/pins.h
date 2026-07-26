#pragma once
// Zentrale Pin-/Adress-Referenz — Elecrow CrowPanel Advance 5.0 (SKU DIS02050A, ESP32-S3-WROOM-1 N16R8)
// Diese Datei ist die Wahrheit für alle Stage-Sketches unter firmware/0X_*/.
// Arduino kompiliert nur Header aus dem eigenen Sketch-Ordner, deshalb liegt in
// jedem Stage-Ordner eine identische Kopie dieser Datei — bei Aenderungen HIER
// zuerst anpassen und dann in jeden Stage-Ordner kopieren.
//
// Quellen: README.md (Abschnitte 3-14) dieses Repos + offizielles Elecrow-Repo
// (LovyanGFX_Driver.h / pins_config.h / SD-Beispiel, Stand Board V1.2/V1.3).

// ---- RGB-Display (ST7262-Panel, parallel) ----------------------------------
#define PIN_LCD_B0   21
#define PIN_LCD_B1   47
#define PIN_LCD_B2   48
#define PIN_LCD_B3   45
#define PIN_LCD_B4   38
#define PIN_LCD_G0    9
#define PIN_LCD_G1   10
#define PIN_LCD_G2   11
#define PIN_LCD_G3   12
#define PIN_LCD_G4   13
#define PIN_LCD_G5   14
#define PIN_LCD_R0    7
#define PIN_LCD_R1   17
#define PIN_LCD_R2   18
#define PIN_LCD_R3    3
#define PIN_LCD_R4   46
#define PIN_LCD_DE   42
#define PIN_LCD_VSYNC 41
#define PIN_LCD_HSYNC 40
#define PIN_LCD_PCLK 39

#define LCD_WIDTH   800
#define LCD_HEIGHT  480
// Elecrows eigener Werks-Testcode (factory_sourcecode) nutzt 21 MHz, NICHT
// die von uns zwischenzeitlich probierten 16/8 MHz (die keine Verbesserung
// brachten, 8 MHz sogar staerkere Instabilitaet). Zurueck auf den offiziell
// validierten Wert.
#define LCD_FREQ_WRITE 21000000

// ---- I2C-Bus (gemeinsam fuer Touch, RTC, Backlight/Buzzer) -----------------
#define PIN_I2C_SDA  15
#define PIN_I2C_SCL  16
#define I2C_FREQ_HZ  400000

// GT911 Touch-Controller: Adresse ist beim Power-On vom Pegel an seinem
// INT-Pin (hier GPIO1) abhaengig. Elecrows Werks-Testcode steuert GPIO1
// AKTIV an (LOW fuer 120ms direkt beim Boot, dann wieder INPUT), um die
// Adresse deterministisch auf 0x5D zu erzwingen ("GT911 上电时序 --->
// 选用 0x5D" -- GT911-Power-On-Sequenz, waehlt 0x5D). Siehe gt911_power_on()
// in touch_probe.h -- MUSS als eine der ersten Aktionen in setup() laufen,
// bevor der GT911 seine eigene Power-On-Reset-Sequenz abschliesst.
// probeGT911Address() bleibt als Sicherheitsnetz erhalten, falls die
// Sequenz aus irgendeinem Grund doch nicht greift.
#define PIN_GT911_INT 1
#define GT911_ADDR_PRIMARY   0x5D
#define GT911_ADDR_SECONDARY 0x14

// PCF8563 Echtzeituhr
#define PCF8563_ADDR 0x51

// STC8H1K28 Backlight-/Buzzer-/Lautsprecher-Controller (Board V1.2/V1.3)
#define BACKLIGHT_BUZZER_ADDR 0x30
#define CMD_BACKLIGHT_MAX   0
#define CMD_BACKLIGHT_MIN   244
#define CMD_BACKLIGHT_OFF   245
#define CMD_BUZZER_ON       246
#define CMD_BUZZER_OFF      247

// ---- SD-Karte (nur bei Function-Select-DIP S1/S0 = 1/1, "MIC & TF Card") ---
#define PIN_SD_MISO   4
#define PIN_SD_SCK    5
#define PIN_SD_MOSI   6
#define PIN_SD_CS     0

// ---- PN532 NFC ueber Software-SPI (frei gesammelte Pins, kein Loeten) ------
// PN532-DIP-Schalter am Modul auf SPI stellen: Sw1=OFF, Sw2=ON.
// Nutzt UART0-Pins (43/44) -> serielle Konsole/USB-CDC darf UART0 nicht
// belegen; Debug-Ausgabe stattdessen aufs Display.
#define PIN_PN532_SCK   43
#define PIN_PN532_MOSI  44
#define PIN_PN532_MISO   2
#define PIN_PN532_SS     8
