// Stage 5 -- PN532 NFC ueber Software-SPI, Erweiterung von Stage 4
// Board: Elecrow CrowPanel Advance 5.0 (DIS02050A), ESP32-S3-WROOM-1 N16R8
//
// FIRMWARE_VERSION (siehe unten, wird auch auf dem Display angezeigt) wird
// bei JEDER inhaltlichen Aenderung an dieser Datei hochgezaehlt -- damit
// nach einem "git pull" + Neu-Flashen auf einen Blick auf dem Panel
// erkennbar ist, ob wirklich die neueste Version laeuft (ohne Serial
// Monitor oder Diff noetig).
//
// Baut auf Stage 4 auf: SD-Karten-Status, RTC-Anzeige und der komplette
// WLAN/ESP-NOW-Status (inkl. Log-Datei) bleiben erhalten (STELLEN/BEEP
// sind auf Nutzerwunsch entfallen, siehe Button-Layout unten). Neu dazu
// kommt der PN532-NFC-Leser: Firmware-Version beim Start, fortlaufendes
// Lesen aufgelegter Karten inkl. DESFire-Tiefenauslesung, sowie 7
// DESFire-Schreibfunktionen (Guthaben-App anlegen/nutzen/loeschen,
// Schluessel wechseln). Der Bildschirm ist jetzt SEHR voll -- bewusst so
// gewaehlt (kompakter statt Funktionen wegzulassen).
//
// AUF NUTZERWUNSCH um 90 Grad gedreht (Hochformat statt Querformat, siehe
// DISPLAY_ROTATION weiter unten) -- NUR in Stage 5, Stufen 1-4 bleiben beim
// urspruenglichen 800x480-Querformat. Das Panel selbst ist als 800x480
// fest verdrahtet (rgb_panel.h); die Drehung passiert rein per
// canvas.setRotation(), inkl. einer manuellen Ruecktransformation der
// GT911-Touch-Rohkoordinaten (rotateTouchToLogical()), da der Touch-Chip
// von dieser Software-Drehung nichts weiss.
//
// VERKABELUNG (siehe README.md Abschnitt 6 -- Pins aus zwei Headern
// zusammengesammelt, kein freier Hardware-SPI-Block, daher Software-SPI):
//   PN532 VCC  -> 3V3   (Wireless-Header)
//   PN532 GND  -> GND   (Wireless-Header)
//   PN532 SCK  -> IO43  (UART0-OUT)
//   PN532 MOSI -> IO44  (UART0-OUT)
//   PN532 MISO -> IO2   (Wireless-Header)
//   PN532 SS   -> IO8   (Wireless-Header)
// PN532-DIP-Schalter am Modul: SPI-Modus, Sw1=OFF, Sw2=ON.
//
// WICHTIG -- da IO43/44 = UART0 TX/RX sind, darf die serielle Konsole nicht
// ueber UART0 laufen, solange der PN532 dort haengt:
//   Arduino IDE: Werkzeuge -> USB CDC On Boot: Enabled
//   PlatformIO: build_flags ARDUINO_USB_MODE/ARDUINO_USB_CDC_ON_BOOT (siehe
//   platformio.ini in platformio/stage5_pn532_spi/)
//
// BENOETIGTE LIBRARY: Adafruit_PN532 (Arduino-IDE-Nutzer: Library Manager),
// MIT ZWEI PATCHES (siehe README.md Abschnitt 3), sonst schlagen laengere
// Antworten (z. B. GetFirmwareVersion bei manchen Modulklonen) fehl:
//   1. In Adafruit_PN532.h:  #define PN532_PACKBUFFSIZ 64   ->  255
//   2. In Adafruit_PN532.cpp, inDataExchange(): Timeout 1000 -> 5000 (ms)
//
// Kartenerkennung + -aktivierung laeuft NUR ueber inListPassiveTarget()
// (kein zusaetzlicher readPassiveTargetID()-Aufruf davor/danach) -- beide
// senden dasselbe native InListPassiveTarget-Kommando (0x4A), und ein
// zweiter Aufruf direkt nach einem bereits erfolgreichen ersten schlaegt an
// echter Hardware regelmaessig fehl, weil die Karte dann schon aktiviert
// ist und auf eine erneute Anticollision/Select-Sequenz nicht mehr
// antwortet (siehe README.md). Die UID kommt stattdessen aus
// desfireGetVersion() (siehe desfireDeepRead()).

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <Adafruit_PN532.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "pins.h"
#include "rgb_panel.h"
#include "touch_standalone.h"
#include "touch_probe.h"
#include "backlight_buzzer.h"
#include "rtc_pcf8563.h"

// Log-Ringpuffer fuers Log-Fenster (siehe drawLogView() weiter unten,
// LOG ANZEIGEN-Button) -- MUSS vor "#include desfire.h" stehen, da
// desfire.h logMsg() statt Serial.print*() nutzt (Aufruf vor Definition
// kompiliert bei PlatformIOs main.cpp -- anders als bei der Arduino-IDE,
// die automatisch Prototypen generiert -- nicht, siehe Kommentar bei
// desfireRunModalAction()). Nur die Textverwaltung + Serial-/SD-Ausgabe
// stehen hier; das eigentliche Zeichnen (drawLogView(), braucht "canvas")
// passiert separat, canvas ist an dieser Stelle noch nicht deklariert.
const int LOG_LINES = 200;
const int LOG_LINE_LEN = 72;
char logBuffer[LOG_LINES][LOG_LINE_LEN];
int logHead = 0;       // naechster Schreibindex (Ringpuffer)
int logCount = 0;      // Anzahl gueltiger Zeilen (bis LOG_LINES)
int logScrollOffset = 0; // 0 = neueste Zeilen sichtbar (ganz "unten")
// Erst true, wenn setup() fertig ist (Canvas/Panel initialisiert) --
// logMsg() zeichnet das Log-Fenster nur dann live nach, sonst wuerde ein
// logMsg()-Aufruf waehrend setup() auf einen noch nicht existierenden
// Framebuffer zugreifen. Vorwaertsdeklarationen: die eigentlichen
// Funktionskoerper (brauchen "canvas" bzw. sdMounted/rtcRead(), die erst
// viel spaeter deklariert sind) kommen weiter unten in der Datei, aber
// logMsg() -- ganz am Anfang der Datei, vor "#include desfire.h" -- muss
// beide schon aufrufen koennen.
bool displayReady = false;
bool logViewOpen = false;
void drawLogView();
void logToSd(const char *line);
// drawLogView() (s.u.) braucht printWrapped() fuers Zeilenumbruch-Layout,
// dessen eigentlicher Funktionskoerper aber (wegen canvas.textWidth())
// selbst erst weiter unten folgt -- ohne diese Vorwaertsdeklaration baut
// PlatformIO's main.cpp nicht (siehe Kommentar oben zu drawLogView()).
int printWrapped(const char *text, int x, int y, int maxWidthPx, int lineHeight);

inline void logAdd(const char *line) {
  strncpy(logBuffer[logHead], line, LOG_LINE_LEN - 1);
  logBuffer[logHead][LOG_LINE_LEN - 1] = 0;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

// Ersetzt Serial.println()/Serial.printf() im gesamten Sketch + in
// desfire.h -- schreibt weiterhin nach Serial (nur falls ein Monitor
// verbunden ist, siehe frueherer Fix), haengt die Zeile an den
// Log-Ringpuffer fuers Log-Fenster an UND an die tagesaktuelle Log-Datei
// auf der SD-Karte (siehe logToSd()).
inline void logMsg(const char *fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = 0;
  }
  if (Serial) Serial.println(buf);
  logAdd(buf);
  logToSd(buf);
  if (displayReady && logViewOpen) drawLogView();
}

#include "desfire.h"

// Fuer echten NTP-Abgleich wifi_secrets.h anlegen (Kopie von
// wifi_secrets.example.h in diesem Ordner, eigene Zugangsdaten eintragen).
// USE_WIFI_NTP_SYNC wird automatisch gesetzt, SOBALD diese Datei
// existiert (__has_include statt einer manuell einzukommentierenden
// #define-Zeile) -- git-ignored/lokal, bleibt also bei jedem "git pull"
// erhalten. FRUEHER musste zusaetzlich "// #define USE_WIFI_NTP_SYNC" in
// DIESER (versionierten) Datei von Hand einkommentiert werden -- das
// wurde bei jedem Pull einer neuen Version wieder auf "auskommentiert"
// zurueckgesetzt, ohne dass es auffiel. Genau DAS war vermutlich der
// Grund, warum WLAN/NTP-SYNC trotz mehrfacher Fixes weiterhin nicht
// funktionierte: die lokale Handaenderung ging bei jedem Update verloren.
#if __has_include("wifi_secrets.h")
#define USE_WIFI_NTP_SYNC
#include <time.h>
#include <WiFiMulti.h>
#include "wifi_secrets.h"
// Erlaubt das Eintragen mehrerer WLAN-Netzwerke in wifi_secrets.h (siehe
// wifi_secrets.example.h) -- WiFiMulti waehlt beim Verbinden automatisch
// das staerkste gerade in Reichweite befindliche Netzwerk aus der Liste,
// statt (wie vorher) fest an EINE SSID/Passwort-Kombination gebunden zu
// sein.
WiFiMulti wifiMulti;

// Diagnose nach jedem wifiMulti.run()-Versuch: bei Erfolg SSID/RSSI/IP,
// bei Fehlschlag zusaetzlich ein WiFi.scanNetworks() (zeigt, was ueberhaupt
// in Reichweite ist) UND die aus wifi_secrets.h konfigurierten SSIDs --
// so laesst sich direkt sehen, ob z. B. ein Tippfehler in der SSID/dem
// Passwort vorliegt oder das Netzwerk schlicht nicht in Reichweite ist.
// Passwoerter werden NICHT geloggt (Sicherheit).
void logWifiAttemptResult() {
  if (WiFi.status() == WL_CONNECTED) {
    logMsg("WLAN: verbunden mit '%s' (RSSI %d dBm), IP %s",
           WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
    return;
  }
  logMsg("WLAN: NICHT verbunden (WiFi.status()=%d).", (int)WiFi.status());
  logMsg("WLAN: konfiguriert sind %u Netzwerk(e):", (unsigned)(sizeof(WIFI_SECRETS) / sizeof(WIFI_SECRETS[0])));
  for (size_t i = 0; i < sizeof(WIFI_SECRETS) / sizeof(WIFI_SECRETS[0]); i++) {
    logMsg("  konfiguriert: '%s'", WIFI_SECRETS[i].ssid);
  }
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    logMsg("WLAN: Scan fand KEIN Netzwerk in Reichweite (evtl. zu weit weg / 5GHz-only-Netzwerk, ESP32 kann nur 2.4GHz).");
  } else {
    logMsg("WLAN: Scan fand %d Netzwerk(e) in Reichweite:", n);
    for (int i = 0; i < n && i < 8; i++) {
      logMsg("  gefunden: '%s' (RSSI %d dBm)", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();
}
#endif

// canvas zeigt per setBuffer() direkt auf den von rgbPanelInit()
// bereitgestellten PSRAM-Framebuffer (esp_lcd_panel_rgb mit Bounce Buffer,
// siehe rgb_panel.h).
LGFX_Sprite canvas;

// Wird bei jeder inhaltlichen Aenderung an dieser Datei hochgezaehlt und
// steht auch auf dem Display (siehe drawStaticParts()) -- so ist nach
// einem "git pull" + Neu-Flashen sofort sichtbar, ob wirklich die
// neueste Version laeuft.
const char *FIRMWARE_VERSION = "2026-07-28.11";

// Panel ist als 800x480-Querformat fest verdrahtet (siehe rgb_panel.h --
// feste RGB-Timings, h_res/v_res = LCD_WIDTH/LCD_HEIGHT). Die 90-Grad-
// Drehung passiert NICHT in diesen Hardware-Timings, sondern rein in der
// Koordinatentransformation von canvas.setRotation() (siehe setup()) --
// canvas.width()/height() liefern danach 480x800 (Hochformat), und alle
// Layout-Konstanten unten sind fuer dieses gedrehte 480x800-Koordinaten-
// system gebaut.
//
// 1 = 90 Grad im Uhrzeigersinn, 3 = 270 Grad (=90 Grad gegen den
// Uhrzeigersinn) -- welcher der beiden Werte am echten, montierten Panel
// dazu fuehrt, dass 0/0 unten links landet (statt oben rechts), laesst
// sich nur am Geraet selbst pruefen. Steht das Bild auf dem Kopf oder
// treffen Touch-Eingaben nicht die Buttons: hier auf 3 aendern -- die
// Touch-Transformation (rotateTouchToLogical() weiter unten) folgt
// automatisch demselben Wert, da dort dieselbe Konstante verwendet wird.
const uint8_t DISPLAY_ROTATION = 1;

SPIClass sdSpi(HSPI);
const char *LOG_FILE_PATH = "/espnow_log.txt";

// Software-SPI-Konstruktor: (SCK, MISO, MOSI, SS) -- vier frei gewaehlte
// Pins, kein Hardware-SPI-Block, siehe README.md Abschnitt 6.
Adafruit_PN532 nfc(PIN_PN532_SCK, PIN_PN532_MISO, PIN_PN532_MOSI, PIN_PN532_SS);

struct Button {
  int x, y, w, h;
  const char *label;
};

// Layout fuer das gedrehte 480 (breit) x 800 (hoch) Koordinatensystem --
// 2 Spalten, da mit den neuen DESFire-Schreibfunktionen viele Buttons
// noetig sind. NTP-SYNC/WLAN-ESP-NOW-Umschalter/BACKLIGHT +/- sind auf
// Nutzerwunsch in ein EINSTELLUNGEN-Untermenue gewandert -- nur noch EIN
// Button dafuer im Hauptbildschirm, dahinter die 7 DESFire-Buttons und
// ganz unten LOG ANZEIGEN (oeffnet das Log-Fenster, siehe drawLogView()).
const int BTN_COL_W = 216, BTN_H = 48, BTN_ROW_GAP = 60;
const int BTN_LEFT_X = 16, BTN_RIGHT_X = 248;
const int BTN_ROW1_Y = 310;

Button btnSettingsOpen   = { BTN_LEFT_X,  BTN_ROW1_Y + 0 * BTN_ROW_GAP, 448,       BTN_H, "EINSTELLUNGEN" };
Button btnSetMasterKey   = { BTN_LEFT_X,  BTN_ROW1_Y + 1 * BTN_ROW_GAP, BTN_COL_W, BTN_H, "MASTER-PW SETZEN" };
Button btnResetMasterKey = { BTN_RIGHT_X, BTN_ROW1_Y + 1 * BTN_ROW_GAP, BTN_COL_W, BTN_H, "AUF STANDARD" };
Button btnCreateApp      = { BTN_LEFT_X,  BTN_ROW1_Y + 2 * BTN_ROW_GAP, BTN_COL_W, BTN_H, "APP ERSTELLEN" };
Button btnDeleteApp      = { BTN_RIGHT_X, BTN_ROW1_Y + 2 * BTN_ROW_GAP, BTN_COL_W, BTN_H, "APP LOESCHEN" };
Button btnCredit         = { BTN_LEFT_X,  BTN_ROW1_Y + 3 * BTN_ROW_GAP, BTN_COL_W, BTN_H, "GUTHABEN BUCHEN" };
Button btnDebit          = { BTN_RIGHT_X, BTN_ROW1_Y + 3 * BTN_ROW_GAP, BTN_COL_W, BTN_H, "GUTHABEN NUTZEN" };
Button btnGetValue       = { BTN_LEFT_X,  BTN_ROW1_Y + 4 * BTN_ROW_GAP, 448,       BTN_H, "GUTHABEN ABFRAGEN" };
Button btnLogOpen        = { BTN_LEFT_X,  BTN_ROW1_Y + 5 * BTN_ROW_GAP, 448,       BTN_H, "LOG ANZEIGEN" };

// EINSTELLUNGEN-Untermenue -- wiederverwendet dieselbe Fenstergeometrie
// wie das DESFire-Aktions-Fenster (MODAL_X/Y/W/H, siehe unten), aber ohne
// Bestaetigungsschritt: NTP-SYNC/WLAN-Umschalter/Backlight sind
// unkritische, sofort rueckgaengig machbare Aktionen, im Gegensatz zu den
// DESFire-Schreibkommandos.
bool settingsOpen = false;

// Custom-AID/-Schluessel fuer die DESFire-Schreibfunktionen -- alles
// hardcoded, da das Panel keine Tastatur/Texteingabe hat (siehe
// firmware/README.md). CUSTOM_KEY zufaellig erzeugt (openssl rand -hex
// 16) -- bei Bedarf hier austauschen. AID LSB-zuerst gespeichert
// (DESFire-Konvention), entspricht menschenlesbar AID 0x123456.
const uint8_t PICC_AID[3]   = { 0x00, 0x00, 0x00 };
const uint8_t CUSTOM_AID[3] = { 0x56, 0x34, 0x12 };
const uint8_t CUSTOM_KEY[16] = {
  0x52, 0xC7, 0xCE, 0x05, 0x82, 0xD4, 0xA3, 0x62,
  0x39, 0xEB, 0xEF, 0xC9, 0x60, 0x34, 0x29, 0xB5
};
const uint8_t VALUE_FILE_NO = 0;
const int32_t VALUE_LOWER_LIMIT = 0;       // Karte lehnt Debit unter 0 selbst ab
const int32_t VALUE_UPPER_LIMIT = 1000000; // willkuerliche, aber grosszuegige Obergrenze
const int32_t CREDIT_DEBIT_AMOUNT = 100;   // fester Betrag pro Tastendruck, leicht aenderbar

// DESFire-Aktions-Fenster: jeder der 7 DESFire-Buttons oeffnet jetzt ein
// eigenes Fenster (auf Nutzerwunsch) statt die Aktion sofort auszufuehren
// -- erst eine Bestaetigung (ABBRECHEN/AUSFUEHREN), danach das Ergebnis
// mit SCHLIESSEN-Button. Solange dieses Fenster offen ist, pausiert der
// automatische Lese-Scan KOMPLETT (siehe loop()) -- kein Wettlauf mehr
// zwischen Auto-Scan und manueller Aktion um dieselbe Anzeige/denselben
// PN532. Das PN532-Modul selbst bleibt dabei jederzeit aktiv (es wird nur
// eben nicht mehr im Hintergrund automatisch abgefragt).
enum DesfireModalState { MODAL_CLOSED, MODAL_CONFIRM, MODAL_RESULT };
DesfireModalState desfireModalState = MODAL_CLOSED;

enum DesfireAction {
  DESFIRE_ACTION_SET_MASTER_KEY = 0,
  DESFIRE_ACTION_RESET_MASTER_KEY,
  DESFIRE_ACTION_CREATE_APP,
  DESFIRE_ACTION_DELETE_APP,
  DESFIRE_ACTION_CREDIT,
  DESFIRE_ACTION_DEBIT,
  DESFIRE_ACTION_GET_VALUE,
  DESFIRE_ACTION_COUNT
};
DesfireAction desfireModalAction = DESFIRE_ACTION_GET_VALUE;

struct DesfireActionInfo {
  const char *title;
  const char *description;
};
const DesfireActionInfo DESFIRE_ACTIONS[DESFIRE_ACTION_COUNT] = {
  { "MASTER-PW SETZEN",  "Setzt Key 0 (PICC-Ebene + eigene App, falls vorhanden) auf den Custom-Schluessel." },
  { "AUF STANDARD",      "Setzt Key 0 (PICC-Ebene + eigene App) zurueck auf den Werks-Default (16 Nullbytes)." },
  { "APP ERSTELLEN",     "Legt Applikation 0x123456 mit Guthaben-Datei an (Start 0)." },
  { "APP LOESCHEN",      "Loescht Applikation 0x123456 UNWIDERRUFLICH." },
  { "GUTHABEN BUCHEN",   "Bucht +100 auf das Guthaben." },
  { "GUTHABEN NUTZEN",   "Zieht -100 vom Guthaben ab (falls genug vorhanden)." },
  { "GUTHABEN ABFRAGEN", "Liest den aktuellen Guthabenstand." },
};

const int MODAL_X = 24, MODAL_Y = 140, MODAL_W = 432, MODAL_H = 460;
const int MODAL_BTN_Y = MODAL_Y + MODAL_H - 66;
Button btnModalCancel  = { MODAL_X + 16, MODAL_BTN_Y, 190, 50, "ABBRECHEN" };
Button btnModalConfirm = { MODAL_X + MODAL_W - 16 - 190, MODAL_BTN_Y, 190, 50, "AUSFUEHREN" };
Button btnModalClose   = { MODAL_X + 16, MODAL_BTN_Y, MODAL_W - 32, 50, "SCHLIESSEN" };

// EINSTELLUNGEN-Untermenue -- gleiche Fenstergeometrie, eigene Buttons in
// einem 2x2-Raster. Nutzt btnModalClose zum Schliessen mit (DESFire-
// Fenster und Einstellungen sind nie gleichzeitig offen).
Button btnSettingsNtpSync    = { MODAL_X + 16, MODAL_Y + 60, 190, 60, "NTP-SYNC" };
Button btnSettingsWifiToggle = { MODAL_X + MODAL_W - 16 - 190, MODAL_Y + 60, 190, 60, "WLAN/ESP-NOW" };
Button btnSettingsBrightUp   = { MODAL_X + 16, MODAL_Y + 60 + 76, 190, 60, "BACKLIGHT +" };
Button btnSettingsBrightDn   = { MODAL_X + MODAL_W - 16 - 190, MODAL_Y + 60 + 76, 190, 60, "BACKLIGHT -" };

// Log-Fenster (auf Nutzerwunsch als EIGENES, groesseres Fenster statt
// einer kleinen, schwer lesbaren Leiste unter den Buttons) -- fast
// bildschirmfuellend, deutlich groessere Schrift als vorher (textSize 1.5
// statt 1) und entsprechend mehr Zeilen gleichzeitig sichtbar. Wird ueber
// den LOG-ANZEIGEN-Button auf dem Hauptbildschirm geoeffnet; waehrenddessen
// pausiert der Hintergrund-Block genau wie beim DESFire-Fenster/
// Einstellungen (siehe loop()).
const int LOGVIEW_X = 8, LOGVIEW_Y = 8, LOGVIEW_W = 464, LOGVIEW_H = 784;
const int LOGVIEW_HEADER_H = 40, LOGVIEW_FOOTER_H = 60;
const int LOGVIEW_LINE_H = 16;
const int LOGVIEW_VISIBLE_LINES = (LOGVIEW_H - LOGVIEW_HEADER_H - LOGVIEW_FOOTER_H) / LOGVIEW_LINE_H;
const int LOGVIEW_BTN_Y = LOGVIEW_Y + LOGVIEW_H - 52;
Button btnLogViewUp    = { LOGVIEW_X + 8, LOGVIEW_BTN_Y, 140, 44, "HOCH" };
Button btnLogViewDown  = { LOGVIEW_X + 8 + 148, LOGVIEW_BTN_Y, 140, 44, "RUNTER" };
Button btnLogViewClose = { LOGVIEW_X + 8 + 148 + 148, LOGVIEW_BTN_Y, 140, 44, "SCHLIESSEN" };

uint8_t backlightPercent = 80;

const uint16_t bgColors[] = { TFT_NAVY, TFT_DARKGREEN, TFT_MAROON, TFT_BLACK };
uint8_t bgIndex = 0;

typedef struct __attribute__((packed)) {
  char     text[48];
  uint32_t counter;
} espnow_message_t;

espnow_message_t outgoing;
espnow_message_t lastReceived;
bool haveReceived = false;
uint8_t lastSrcMac[6] = {0};
bool lastSendOk = false;
bool haveSendResult = false;
uint32_t lastSendMs = 0;

// onDataRecv() laeuft im WiFi/ESP-NOW-System-Task, nicht im loop()-Task --
// siehe firmware/README.md Abschnitt 7. Nur Flag setzen, Zeichnen/Loggen
// passieren ausschliesslich in loop().
volatile bool recvPending = false;
const uint32_t SEND_INTERVAL_MS = 2000;
const uint32_t RTC_UPDATE_INTERVAL_MS = 1000;
const uint32_t SD_POLL_INTERVAL_MS = 2000;
const uint32_t PN532_POLL_INTERVAL_MS = 300;
const uint32_t PN532_DEBOUNCE_MS = 1000;

uint8_t broadcastAddr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// SD-Status: siehe checkSdCard() weiter unten -- erkennt Einstecken/
// Herausziehen waehrend des Betriebs.
bool sdMounted = false;
bool sdCheckedOnce = false;
char sdStatusMsg[64] = "SD-Karte: wird geprueft...";
uint16_t sdStatusColor = TFT_LIGHTGREY;

// PN532-Status: einmaliger Verbindungsversuch in setup() (siehe
// pn532Begin()) -- anders als bei der SD-Karte kein Live-Hotplug-Polling,
// da das fuer diesen Basistest nicht gefordert war.
bool pn532Ready = false;
char pn532StatusMsg[64] = "PN532: wird geprueft...";
uint16_t pn532StatusColor = TFT_LIGHTGREY;
char uidMsg[64] = "Noch keine Karte gelesen.";
// DESFire-Tiefenauslesung (siehe desfire.h + desfireDeepRead() unten) --
// Details landen in DESFIRE_LOG_PATH auf der SD-Karte, hier nur die kurze
// Ein-Zeilen-Zusammenfassung fuers Display.
char desfireSummaryMsg[80] = "";
const char *DESFIRE_LOG_PATH = "/desfire_log.txt";
uint32_t lastPn532AttemptMs = 0;
uint32_t lastUidReadMs = 0;

// WLAN-IP -- WiFi.begin() passiert in dieser Stage ausschliesslich
// innerhalb von syncFromNtp() (ESP-NOW braucht dafuer keine AP-
// Verbindung), daher gibt es sonst keinen dauerhaften Verbindungsstatus.
// Dient als Diagnose fuer NTP-SYNC: zeigt, ob ueberhaupt eine IP bezogen
// wurde, auch wenn der anschliessende NTP-Request selbst fehlschlaegt.
char wifiIpMsg[48] = "WLAN: nicht verbunden";
uint16_t wifiIpColor = TFT_LIGHTGREY;
// Umschalter WLAN/ESP-NOW (siehe btnSettingsWifiToggle-Handler in loop(),
// im EINSTELLUNGEN-Untermenue): false =
// ESP-NOW-Broadcast-Modus (Default, keine AP-Verbindung), true = mit dem
// AP verbunden (fuer NTP-SYNC). WiFi.mode(WIFI_STA) bleibt in beiden
// Faellen gesetzt -- ESP-NOW braucht das, aber keine AP-Assoziation.
bool wlanModeActive = false;
const uint32_t WIFI_POLL_INTERVAL_MS = 1000;
uint32_t lastWifiPollMs = 0;
// Reconnect-Versuche via WiFiMulti (siehe loop()) -- deutlich seltener als
// WIFI_POLL_INTERVAL_MS, da wifiMulti.run() bei fehlender Verbindung selbst
// mit kurzem Timeout noch einen blockierenden Scan durchfuehrt (WiFi.
// scanNetworks() laesst sich nicht vermeiden) und das sonst staendig
// ESP-NOW/Touch-Reaktionszeit stoeren wuerde.
const uint32_t WIFI_MULTI_RETRY_INTERVAL_MS = 15000;
uint32_t lastWifiMultiRetryMs = 0;

// Bereichsgrenzen der einzelnen Update-Funktionen -- an einer Stelle
// definiert, damit das fillRect() in der jeweiligen Funktion garantiert
// mit sich selbst konsistent bleibt. Fuer 480px Breite (siehe
// DISPLAY_ROTATION oben) gestapelt statt wie zuvor auf 800px verteilt.
const int SD_INFO_Y = 76, SD_INFO_H = 18;
const int RTC_INFO_Y = 96, RTC_INFO_H = 32;
const int PN532_INFO_Y = 132, PN532_INFO_H = 18;
const int SEND_INFO_Y = 154, SEND_INFO_H = 34;
const int RECV_INFO_Y = 192, RECV_INFO_H = 34;
// UID_INFO jetzt zweizeilig: UID selbst + DESFire-Zusammenfassung darunter.
const int UID_INFO_Y = 230, UID_INFO_H = 40;
// Liegt in der vorher freien Luecke zwischen UID_INFO (endet bei 270) und
// den Buttons (beginnen bei 336) -- kein anderer Bereich musste verschoben
// werden.
const int WIFI_INFO_Y = 276, WIFI_INFO_H = 20;

void drawButton(const Button &b, uint16_t fill) {
  canvas.fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
  canvas.drawRoundRect(b.x, b.y, b.w, b.h, 8, TFT_WHITE);
  // Schwarz statt Weiss -- auf dem hellen (TFT_DARKGREY wirkt auf diesem
  // Panel deutlich heller als am Schreibtisch erwartet) bzw. gruenen
  // Button-Fuellgrund war weisse Schrift kaum lesbar.
  canvas.setTextColor(TFT_BLACK);
  // 1.5 statt 2 -- die 2-spaltigen 216px-breiten Buttons (11 Stueck, siehe
  // Button-Layout oben) brauchen bei den laengeren Labels ("MASTER-PW
  // SETZEN" etc.) etwas mehr Luft, als textSize(2) noch zulassen wuerde.
  canvas.setTextSize(1.5);
  canvas.setTextDatum(lgfx::middle_center);
  canvas.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
}

bool inside(const Button &b, int32_t x, int32_t y) {
  return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

// Wandelt eine ROHE Touch-Koordinate (physisches 800x480-Panel-Raster, wie
// vom GT911 geliefert -- touch_standalone.h wendet KEINE Rotation an,
// siehe dortiger Kopfkommentar "reine Identitaetsabbildung") in die
// GEDREHTE logische Bildschirm-Koordinate (480x800) um, in der die Button-
// Layouts oben definiert sind. Formeln entsprechen 1:1 der Adafruit_GFX-/
// LovyanGFX-Rotationskonvention (aus GFXcanvas::drawPixel() zurueck-
// gerechnet) -- folgt automatisch DISPLAY_ROTATION oben.
inline void rotateTouchToLogical(int32_t &x, int32_t &y) {
  int32_t rawX = x, rawY = y;
  if (DISPLAY_ROTATION == 1) {
    x = rawY;
    y = LCD_WIDTH - 1 - rawX;
  } else if (DISPLAY_ROTATION == 3) {
    x = LCD_HEIGHT - 1 - rawY;
    y = rawX;
  }
}

// Titel + eigene MAC-Adresse + alle fuenf Buttons -- aendert sich nur, wenn
// sich die Hintergrundfarbe aendert (Tap auf freie Flaeche).
void drawStaticParts() {
  canvas.fillScreen(bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(3);
  canvas.setCursor(12, 12);
  canvas.println("CrowPanel Advance 5.0");
  // Hochformat ist nur 480px breit statt vormals 800px -- laengere
  // Zeilen (Untertitel, MAC-Adresse) laufen bei textSize(2) aus dem Bild,
  // deshalb hier textSize(1.5) statt (2) (LovyanGFX erlaubt Fliesskomma-
  // Groessen, nicht nur ganze Zahlen wie klassisches Adafruit_GFX).
  canvas.setTextSize(1.5);
  canvas.setCursor(12, 40);
  canvas.println("Stage 5: PN532 NFC + SD + RTC + WLAN/ESP-NOW");

  canvas.setCursor(12, 56);
  canvas.print("Eigene MAC: ");
  canvas.print(WiFi.macAddress());
  canvas.printf("  v%s", FIRMWARE_VERSION);

  drawButton(btnSettingsOpen, TFT_DARKGREY);
  drawButton(btnSetMasterKey, TFT_DARKGREY);
  drawButton(btnResetMasterKey, TFT_DARKGREY);
  drawButton(btnCreateApp, TFT_DARKGREY);
  drawButton(btnDeleteApp, TFT_DARKGREY);
  drawButton(btnCredit, TFT_DARKGREY);
  drawButton(btnDebit, TFT_DARKGREY);
  drawButton(btnGetValue, TFT_DARKGREY);
  drawButton(btnLogOpen, TFT_DARKGREY);
}

// Nur die SD-Statuszeile -- wird ausschliesslich bei einem tatsaechlichen
// Zustandswechsel (Karte erkannt/entfernt) aus checkSdCard() aufgerufen.
void updateSdInfo() {
  canvas.fillRect(0, SD_INFO_Y, canvas.width(), SD_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(1.5);
  canvas.setTextColor(sdStatusColor);
  canvas.setCursor(12, SD_INFO_Y + 2);
  canvas.print(sdStatusMsg);
}

// Nur die RTC-Zeile -- wird jede Sekunde aus loop() aufgerufen, unabhaengig
// von SD-/PN532-/ESP-NOW-Updates (eigener Bildschirmbereich).
void updateRtcInfo() {
  canvas.fillRect(0, RTC_INFO_Y, canvas.width(), RTC_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(3);
  canvas.setCursor(12, RTC_INFO_Y);

  RtcDateTime dt;
  char buf[64];
  if (rtcRead(dt)) {
    rtcFormat(dt, buf, sizeof(buf));
    canvas.setTextColor(dt.voltageLow ? TFT_RED : TFT_GREENYELLOW);
    canvas.print(buf);
  } else {
    canvas.setTextColor(TFT_RED);
    canvas.print("RTC-Lesefehler (I2C pruefen)");
  }
}

// Nur die PN532-Statuszeile -- wird einmalig nach pn532Begin() gezeichnet
// (kein Live-Hotplug-Polling wie bei der SD-Karte, siehe oben).
void updatePn532Info() {
  canvas.fillRect(0, PN532_INFO_Y, canvas.width(), PN532_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(1.5);
  canvas.setTextColor(pn532StatusColor);
  canvas.setCursor(12, PN532_INFO_Y + 2);
  canvas.print(pn532StatusMsg);
}

// Nur Sendezaehler + letzter Sendestatus -- wird ausschliesslich vom
// 2s-Sendezyklus in loop() aufgerufen.
void updateSendInfo() {
  canvas.fillRect(0, SEND_INFO_Y, canvas.width(), SEND_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(1.5);

  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(12, SEND_INFO_Y + 2);
  canvas.printf("ESP-NOW gesendet: %lu\n", (unsigned long)outgoing.counter);

  canvas.setCursor(12, SEND_INFO_Y + 18);
  if (haveSendResult) {
    canvas.setTextColor(lastSendOk ? TFT_GREENYELLOW : TFT_RED);
    canvas.printf("Letzter Sendestatus: %s\n", lastSendOk ? "OK" : "FEHLER");
  } else {
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.println("Letzter Sendestatus: (noch keiner)");
  }
}

// Nur die zuletzt empfangene Nachricht -- wird ausschliesslich aus
// onDataRecv() (ueber recvPending, siehe loop()) aufgerufen.
void updateReceivedInfo() {
  canvas.fillRect(0, RECV_INFO_Y, canvas.width(), RECV_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(1.5);
  canvas.setTextColor(TFT_GREENYELLOW);
  canvas.setCursor(12, RECV_INFO_Y + 2);
  if (haveReceived) {
    canvas.printf("Empfangen von %02X:%02X:%02X:%02X:%02X:%02X:\n",
                  lastSrcMac[0], lastSrcMac[1], lastSrcMac[2],
                  lastSrcMac[3], lastSrcMac[4], lastSrcMac[5]);
    canvas.setCursor(12, RECV_INFO_Y + 18);
    canvas.printf("\"%s\" (Zaehler %lu)", lastReceived.text, (unsigned long)lastReceived.counter);
  } else {
    canvas.println("Noch keine ESP-NOW-Nachricht empfangen.");
  }
}

// UID-Zeile + darunter eine kurze DESFire-Zusammenfassung (Details stehen
// in /desfire_log.txt auf der SD-Karte, siehe desfireDeepRead()) -- wird
// ausschliesslich aus loop()s PN532-Polling aufgerufen.
void updateUidInfo() {
  canvas.fillRect(0, UID_INFO_Y, canvas.width(), UID_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(1.5);
  canvas.setTextColor(TFT_GREENYELLOW);
  canvas.setCursor(12, UID_INFO_Y + 2);
  canvas.print(uidMsg);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setCursor(12, UID_INFO_Y + 20);
  canvas.print(desfireSummaryMsg);
}

// Nur die WLAN-IP-Zeile -- wird initial in setup() und nach jedem
// NTP-SYNC-Versuch (siehe loop()) neu geschrieben. Zeigt auch dann eine
// IP an, wenn WiFi.begin() erfolgreich war, der anschliessende NTP-Request
// selbst aber fehlschlaegt -- hilft zu unterscheiden, ob ueberhaupt eine
// WLAN-Verbindung zustande kam oder schon das WLAN selbst das Problem ist.
void updateWifiInfo() {
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(wifiIpMsg, sizeof(wifiIpMsg), "WLAN-IP: %s", WiFi.localIP().toString().c_str());
    wifiIpColor = TFT_GREENYELLOW;
  } else {
    snprintf(wifiIpMsg, sizeof(wifiIpMsg), "WLAN: nicht verbunden");
    wifiIpColor = TFT_LIGHTGREY;
  }
  canvas.fillRect(0, WIFI_INFO_Y, canvas.width(), WIFI_INFO_H, bgColors[bgIndex]);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(1.5);
  canvas.setTextColor(wifiIpColor);
  canvas.setCursor(12, WIFI_INFO_Y + 2);
  canvas.print(wifiIpMsg);
}

// Zeichnet den kompletten Hauptbildschirm neu -- gebraucht beim Tap auf
// freie Flaeche (Hintergrundfarbe wechseln) UND beim Schliessen/Abbrechen
// eines DESFire-Aktions-Fensters (das den Hauptbildschirm komplett
// ueberdeckt hatte).
void redrawMainScreen() {
  drawStaticParts();
  updateSdInfo();
  updateRtcInfo();
  updatePn532Info();
  updateSendInfo();
  updateReceivedInfo();
  updateUidInfo();
  updateWifiInfo();
}

// Zeichnet das (fast bildschirmfuellende) Log-Fenster: Kopfzeile,
// aktuell sichtbare Zeilen aus dem Ringpuffer (siehe logScrollOffset),
// HOCH/RUNTER/SCHLIESSEN. Wird von logMsg() automatisch nach jeder neuen
// Zeile aufgerufen, SOLANGE das Fenster gerade offen ist (logViewOpen) --
// live mitlesbar, was waehrend einer Aktion passiert, ohne das Fenster
// vorher schliessen zu muessen.
void drawLogView() {
  canvas.fillRoundRect(LOGVIEW_X, LOGVIEW_Y, LOGVIEW_W, LOGVIEW_H, 12, TFT_BLACK);
  canvas.drawRoundRect(LOGVIEW_X, LOGVIEW_Y, LOGVIEW_W, LOGVIEW_H, 12, TFT_WHITE);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(LOGVIEW_X + 12, LOGVIEW_Y + 8);
  canvas.printf("LOG (%d/%d)", logCount - logScrollOffset, logCount);

  canvas.setTextSize(1.5);
  canvas.setTextColor(TFT_GREENYELLOW);
  int textTop = LOGVIEW_Y + LOGVIEW_HEADER_H;
  int textBottom = LOGVIEW_Y + LOGVIEW_H - LOGVIEW_FOOTER_H;
  int textMaxWidth = LOGVIEW_W - 24; // 12px Rand links + rechts
  int endIdx = logCount - logScrollOffset; // exklusiv
  // LOGVIEW_VISIBLE_LINES ist hier nur eine OBERE Grenze fuers Fenster
  // (jeder Eintrag mind. 1 Zeile) -- tatsaechlich gezeichnet wird unten
  // ueber printWrapped() mit variabler Zeilenzahl pro Eintrag, damit lange
  // Meldungen (bis zu LOG_LINE_LEN=72 Zeichen) nicht mit der naechsten
  // Meldung ineinanderlaufen, wie es beim vorherigen fixen
  // LOGVIEW_LINE_H-pro-Eintrag-Layout passierte.
  int startIdx = endIdx - LOGVIEW_VISIBLE_LINES;
  if (startIdx < 0) startIdx = 0;
  int yCursor = textTop;
  for (int i = startIdx; i < endIdx; i++) {
    if (yCursor >= textBottom) break;
    int bufIdx = (logHead - logCount + i + LOG_LINES * 2) % LOG_LINES;
    int linesUsed = printWrapped(logBuffer[bufIdx], LOGVIEW_X + 12, yCursor, textMaxWidth, LOGVIEW_LINE_H);
    yCursor += linesUsed * LOGVIEW_LINE_H;
  }

  drawButton(btnLogViewUp, TFT_DARKGREY);
  drawButton(btnLogViewDown, TFT_DARKGREY);
  drawButton(btnLogViewClose, TFT_DARKGREY);
}

// EINSTELLUNGEN-Untermenue: NTP-SYNC/WLAN-Umschalter/Backlight, ohne
// Bestaetigungsschritt (unkritische Aktionen, siehe Deklaration oben).
void drawSettingsMenu() {
  canvas.fillRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 12, TFT_NAVY);
  canvas.drawRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 12, TFT_WHITE);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(2);
  canvas.setCursor(MODAL_X + 16, MODAL_Y + 16);
  canvas.println("EINSTELLUNGEN");
  drawButton(btnSettingsNtpSync, TFT_DARKGREY);
  drawButton(btnSettingsWifiToggle, TFT_DARKGREY);
  drawButton(btnSettingsBrightUp, TFT_DARKGREY);
  drawButton(btnSettingsBrightDn, TFT_DARKGREY);
  drawButton(btnModalClose, TFT_DARKGREY);
}

// Gibt Text zeilenweise aus, bricht zwischen Woertern um, damit jede
// Zeile innerhalb von maxWidthPx bleibt (canvas.textWidth() misst bei der
// AKTUELL per setTextSize()/setTextColor() usw. gesetzten Textgroesse --
// muss also VOR dem Aufruf gesetzt sein). Gebraucht fuer die
// Aktionsbeschreibungen im DESFire-Bestaetigungsfenster, die bei
// textSize(1.5) sonst weit ueber den 432px breiten Fensterrand
// hinauslaufen wuerden. Gibt die Anzahl gezeichneter Zeilen zurueck.
int printWrapped(const char *text, int x, int y, int maxWidthPx, int lineHeight) {
  char buf[160];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char line[160] = "";
  int lineCount = 0;
  char *word = strtok(buf, " ");
  while (word) {
    char candidate[160];
    if (line[0] == 0) snprintf(candidate, sizeof(candidate), "%s", word);
    else snprintf(candidate, sizeof(candidate), "%s %s", line, word);
    if (line[0] != 0 && canvas.textWidth(candidate) > maxWidthPx) {
      canvas.setCursor(x, y + lineCount * lineHeight);
      canvas.println(line);
      lineCount++;
      snprintf(line, sizeof(line), "%s", word);
    } else {
      snprintf(line, sizeof(line), "%s", candidate);
    }
    word = strtok(nullptr, " ");
  }
  if (line[0] != 0) {
    canvas.setCursor(x, y + lineCount * lineHeight);
    canvas.println(line);
    lineCount++;
  }
  return lineCount;
}

// Rahmen + Titel des DESFire-Aktions-Fensters -- gemeinsame Basis fuer
// Bestaetigungs- und Ergebnis-Ansicht (siehe DesfireModalState oben).
void drawDesfireModalFrame() {
  canvas.fillRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 12, TFT_NAVY);
  canvas.drawRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 12, TFT_WHITE);
  canvas.setTextDatum(lgfx::top_left);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(2);
  canvas.setCursor(MODAL_X + 16, MODAL_Y + 16);
  canvas.println(DESFIRE_ACTIONS[desfireModalAction].title);
}

// Bestaetigungs-Ansicht: Beschreibung + ABBRECHEN/AUSFUEHREN.
void drawDesfireModalConfirm() {
  drawDesfireModalFrame();
  canvas.setTextSize(1.5);
  canvas.setTextColor(TFT_LIGHTGREY);
  int descLines = printWrapped(DESFIRE_ACTIONS[desfireModalAction].description,
                                MODAL_X + 16, MODAL_Y + 60, MODAL_W - 32, 20);
  canvas.setTextColor(TFT_YELLOW);
  canvas.setCursor(MODAL_X + 16, MODAL_Y + 60 + descLines * 20 + 16);
  canvas.println("Karte auflegen, dann AUSFUEHREN tippen.");
  drawButton(btnModalCancel, TFT_DARKGREY);
  drawButton(btnModalConfirm, TFT_DARKGREY);
}

// Ergebnis-Ansicht: Ergebnistext (desfireSummaryMsg, von der jeweiligen
// desfireOpXxx()-Funktion gesetzt) + SCHLIESSEN.
void drawDesfireModalResult() {
  drawDesfireModalFrame();
  canvas.setTextSize(1.5);
  canvas.setTextColor(TFT_GREENYELLOW);
  printWrapped(desfireSummaryMsg, MODAL_X + 16, MODAL_Y + 60, MODAL_W - 32, 20);
  drawButton(btnModalClose, TFT_DARKGREY);
}

// Aktuelle RTC-Zeit als "YYYY-MM-DD hh:mm:ss" -- fuer Log-Zeitstempel.
// Getrennt von rtcFormat() (die haengt zusaetzlich den VL-Hinweis an, den
// wir im Log nicht wollen).
void formatTimestamp(char *buf, size_t bufLen) {
  RtcDateTime dt;
  if (rtcRead(dt)) {
    snprintf(buf, bufLen, "%04u-%02u-%02u %02u:%02u:%02u",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  } else {
    snprintf(buf, bufLen, "0000-00-00 00:00:00"); // RTC nicht lesbar
  }
}

// Haengt eine Log-Zeile mit RTC-Zeitstempel an eine tagesaktuelle Datei
// auf der SD-Karte an (z. B. /log_2026-07-28.txt) -- neue Datei
// automatisch bei Datumswechsel, da der Dateiname das Datum enthaelt.
// No-op, solange keine SD-Karte gemountet ist (sdMounted, siehe
// checkSdCard()) -- wird bei jedem logMsg()-Aufruf versucht, holt also
// von selbst auf, sobald eine Karte eingesteckt wird.
void logToSd(const char *line) {
  if (!sdMounted) return;
  RtcDateTime dt;
  char path[40];
  if (rtcRead(dt)) {
    snprintf(path, sizeof(path), "/log_%04u-%02u-%02u.txt", dt.year, dt.month, dt.day);
  } else {
    snprintf(path, sizeof(path), "/log_unbekanntes_datum.txt");
  }
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  char ts[32];
  formatTimestamp(ts, sizeof(ts));
  f.printf("%s  %s\n", ts, line);
  f.close();
}

// Wird nur beim Uebergang "nicht gemountet -> gemountet" aufgerufen --
// haengt eine Startmarke an die Log-Datei an (legt sie bei Bedarf an).
void logSessionStart() {
  File f = SD.open(LOG_FILE_PATH, FILE_APPEND);
  if (!f) {
    logMsg("logSessionStart(): SD.open() fehlgeschlagen");
    return;
  }
  char ts[32];
  formatTimestamp(ts, sizeof(ts));
  f.printf("--- Log gestartet %s ---\n", ts);
  f.close();
  logMsg("logSessionStart(): Startmarke geschrieben.");
}

// Haengt die zuletzt empfangene ESP-NOW-Nachricht mit RTC-Zeitstempel an
// die Log-Datei an. Wird nur aufgerufen, wenn sdMounted gerade true ist --
// schlaegt der Schreibzugriff trotzdem fehl (Karte just in dem Moment
// herausgezogen), wird das stillschweigend uebersprungen; der naechste
// checkSdCard()-Durchlauf korrigiert den angezeigten Status.
void logReceivedMessage() {
  if (!sdMounted) return;
  File f = SD.open(LOG_FILE_PATH, FILE_APPEND);
  if (!f) return;
  char ts[32];
  formatTimestamp(ts, sizeof(ts));
  f.printf("%s  %02X:%02X:%02X:%02X:%02X:%02X  \"%s\"  Zaehler %lu\n",
           ts, lastSrcMac[0], lastSrcMac[1], lastSrcMac[2],
           lastSrcMac[3], lastSrcMac[4], lastSrcMac[5],
           lastReceived.text, (unsigned long)lastReceived.counter);
  f.close();
}

// Erkennt Einstecken/Herausziehen waehrend des Betriebs, OHNE bei jedem
// Poll-Zyklus einen teuren Vollremount zu machen (siehe firmware/README.md
// Abschnitt 7 -- eine fruehere Version machte das alle 2s unbedingt, was
// bei manchen Karten spuerbar lange blockierte). Solange die Karte als
// gemountet gilt, reicht ein guenstiger Lebendigkeits-Check (Log-Datei kurz
// oeffnen+schliessen, ohne zu schreiben). Zeichnet nur bei einem
// tatsaechlichen Zustandswechsel neu.
void checkSdCard() {
  if (sdMounted) {
    File probe = SD.open(LOG_FILE_PATH, FILE_APPEND);
    if (probe) {
      probe.close();
      return; // weiterhin da, nichts zu tun
    }
    logMsg("checkSdCard(): Log-Datei nicht mehr erreichbar -- Karte vermutlich entfernt.");
    SD.end();
    sdSpi.end();
    sdMounted = false;
    snprintf(sdStatusMsg, sizeof(sdStatusMsg), "SD-Karte: entfernt");
    sdStatusColor = TFT_RED;
    updateSdInfo();
    return;
  }

  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  bool ok = SD.begin(PIN_SD_CS, sdSpi, 80000000);
  logMsg("checkSdCard(): SD.begin() -> %s\n", ok ? "true" : "false");

  if (!ok) {
    if (!sdCheckedOnce) {
      snprintf(sdStatusMsg, sizeof(sdStatusMsg), "SD-Karte: nicht eingesteckt/nicht lesbar");
      sdStatusColor = TFT_LIGHTGREY;
      updateSdInfo();
      sdCheckedOnce = true;
    }
    return;
  }

  sdMounted = true;
  sdCheckedOnce = true;

  uint8_t cardType = SD.cardType();
  const char *typeStr = "unbekannt";
  if (cardType == CARD_MMC) typeStr = "MMC";
  else if (cardType == CARD_SD) typeStr = "SDSC";
  else if (cardType == CARD_SDHC) typeStr = "SDHC";
  snprintf(sdStatusMsg, sizeof(sdStatusMsg), "SD-Karte: erkannt (%s, %llu MB)",
           typeStr, SD.cardSize() / (1024ULL * 1024ULL));
  sdStatusColor = TFT_GREENYELLOW;
  logMsg("checkSdCard(): Zustandswechsel -> gemountet, %s\n", sdStatusMsg);
  logSessionStart();
  buzzerBeep(80);
  updateSdInfo();
}

// Einmaliger Verbindungsversuch (siehe pn532Ready oben) -- Firmware-Version
// abfragen und SAM auf normalen Lesemodus konfigurieren.
bool pn532Begin() {
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    snprintf(pn532StatusMsg, sizeof(pn532StatusMsg), "PN532: nicht gefunden (Verkabelung/DIP pruefen)");
    pn532StatusColor = TFT_RED;
    logMsg("pn532Begin(): getFirmwareVersion() -> 0 (nicht gefunden)");
    return false;
  }
  snprintf(pn532StatusMsg, sizeof(pn532StatusMsg), "PN532: PN5%02X gefunden, Firmware %d.%d",
           (unsigned)((versiondata >> 24) & 0xFF), (unsigned)((versiondata >> 16) & 0xFF),
           (unsigned)((versiondata >> 8) & 0xFF));
  pn532StatusColor = TFT_GREENYELLOW;
  logMsg("pn532Begin(): %s\n", pn532StatusMsg);
  nfc.SAMConfig();
  // Ohne das wuerde der PN532-CHIP SELBST (nicht die Bibliothek) bei
  // inListPassiveTarget() intern unbegrenzt oft nach einer Karte suchen,
  // solange keine da ist (RFConfiguration-Default fuer
  // MxRtyPassiveActivation = 0xFF = "endlos"), bevor er ueberhaupt eine
  // Antwort an den Host zurueckschickt -- das haelt loop() so lange auf,
  // wie die Karte fehlt (siehe loop()-Kommentar bei inListPassiveTarget()).
  // 1 = nur ein einziger Versuch, dann sofort "nichts gefunden"
  // zurueckmelden; der naechste 300ms-Poll-Zyklus versucht es einfach
  // erneut, eine gerade erst aufgelegte Karte wird dadurch nicht verpasst.
  nfc.setPassiveActivationRetries(1);
  return true;
}

// Fuehrt einen vollstaendigen DESFire-Lesevorgang durch: GetVersion, Liste
// der Anwendungen (AIDs), pro Anwendung Authentifizierung mit dem Default-
// Schluessel (16 Nullbytes, probiert erst 2K3DES dann AES -- siehe
// desfire.h) + Datei-Liste + Dateiinhalte wo moeglich (Enciphered-Dateien
// werden erkannt, aber nicht entschluesselt -- siehe Kopfkommentar in
// desfire.h). Alle Details landen mit RTC-Zeitstempel in DESFIRE_LOG_PATH
// auf der SD-Karte (nur wenn sie gerade gemountet ist); desfireSummaryMsg
// bekommt eine kurze Ein-Zeilen-Zusammenfassung fuers Display.
//
// WICHTIG: Dieser Code wurde an keiner echten DESFire-Karte getestet
// (siehe desfire.h). Schlaegt die Authentifizierung fehl, ist die
// wahrscheinlichste Erklaerung, dass die Karte nicht mehr die
// Werksschluessel hat (normal bei den meisten Karten aus dem echten
// Einsatz) -- kein Grund zur Sorge, kein Bug.
void desfireDeepRead() {
  File log;
  bool haveLog = false;
  if (sdMounted) {
    log = SD.open(DESFIRE_LOG_PATH, FILE_APPEND);
    haveLog = (bool)log;
    if (haveLog) {
      char ts[32];
      formatTimestamp(ts, sizeof(ts));
      log.printf("=== DESFire-Lesevorgang %s ===\n", ts);
    } else {
      logMsg("desfireDeepRead(): SD.open() fehlgeschlagen, logge nur nach Serial.");
    }
  }

  DesfireVersion ver;
  if (!desfireGetVersion(ver)) {
    snprintf(uidMsg, sizeof(uidMsg), "Karte erkannt (kein DESFire / Kommunikationsfehler)");
    snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Kein DESFire (GetVersion fehlgeschlagen)");
    if (haveLog) {
      log.println("GetVersion fehlgeschlagen -- keine DESFire-Karte oder Kommunikationsfehler.");
      log.close();
    }
    return;
  }

  // UID kommt aus der DESFire-eigenen GetVersion-Antwort, nicht aus einem
  // separaten readPassiveTargetID()-Aufruf (siehe Kopfkommentar der Datei).
  snprintf(uidMsg, sizeof(uidMsg), "Karte erkannt, UID: %02X %02X %02X %02X %02X %02X %02X",
           ver.uid[0], ver.uid[1], ver.uid[2], ver.uid[3], ver.uid[4], ver.uid[5], ver.uid[6]);

  if (haveLog) {
    log.printf("HW: Vendor=0x%02X Type=0x%02X SubType=0x%02X Version=%u.%u StorageSize=0x%02X Protocol=0x%02X\n",
               ver.hwVendorId, ver.hwType, ver.hwSubType, ver.hwMajorVersion, ver.hwMinorVersion,
               ver.hwStorageSize, ver.hwProtocol);
    log.printf("SW: Vendor=0x%02X Type=0x%02X SubType=0x%02X Version=%u.%u StorageSize=0x%02X Protocol=0x%02X\n",
               ver.swVendorId, ver.swType, ver.swSubType, ver.swMajorVersion, ver.swMinorVersion,
               ver.swStorageSize, ver.swProtocol);
    log.printf("UID: %02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
               ver.uid[0], ver.uid[1], ver.uid[2], ver.uid[3], ver.uid[4], ver.uid[5], ver.uid[6]);
    // productionWeek/-year sind BCD-kodiert (NXP-Konvention), NICHT
    // Binaerzahlen -- Byte 0x27 bedeutet Woche 27, nicht 39. Ohne
    // BCD-Dekodierung zeigte das Log bisher z. B. "Woche 39 / 2037" statt
    // korrekt "Woche 27 / 2025" (an echter Hardware aufgefallen, siehe
    // firmware/README.md).
    uint8_t prodWeek = ((ver.productionWeek >> 4) & 0x0F) * 10 + (ver.productionWeek & 0x0F);
    uint8_t prodYear = ((ver.productionYear >> 4) & 0x0F) * 10 + (ver.productionYear & 0x0F);
    log.printf("Batch: %02X%02X%02X%02X%02X, Produktion: Woche %u / 20%02u\n",
               ver.batchNo[0], ver.batchNo[1], ver.batchNo[2], ver.batchNo[3], ver.batchNo[4],
               prodWeek, prodYear);
  }

  uint8_t aids[28][3];
  uint8_t appCount = desfireGetApplicationIDs(aids, 28);
  if (haveLog) log.printf("Anwendungen (AIDs): %u\n", appCount);

  uint8_t authedApps = 0;
  for (uint8_t a = 0; a < appCount; a++) {
    if (haveLog) log.printf("-- AID %02X%02X%02X --\n", aids[a][2], aids[a][1], aids[a][0]);
    if (!desfireSelectApplication(aids[a])) {
      if (haveLog) log.println("   SelectApplication fehlgeschlagen, ueberspringe.");
      continue;
    }

    uint8_t sessionKey[16];
    const char *cipherName;
    if (!desfireAuthDefaultKey(0, sessionKey, &cipherName)) {
      if (haveLog) log.println("   Authentifizierung mit Default-Schluessel (2K3DES/AES) fehlgeschlagen -- "
                                "Karte hat vermutlich andere Schluessel.");
      continue;
    }
    authedApps++;
    if (haveLog) log.printf("   Authentifiziert mit Default-Schluessel (%s).\n", cipherName);

    uint8_t fileIds[32];
    uint8_t fileCount = desfireGetFileIDs(fileIds, 32);
    if (haveLog) log.printf("   Dateien: %u\n", fileCount);

    for (uint8_t f = 0; f < fileCount; f++) {
      DesfireFileSettings fs;
      if (!desfireGetFileSettings(fileIds[f], fs)) {
        if (haveLog) log.printf("   Datei %u: GetFileSettings fehlgeschlagen.\n", fileIds[f]);
        continue;
      }
      const char *typeName = fs.fileType == 0x00 ? "Standard Data"
                            : fs.fileType == 0x01 ? "Backup Data"
                            : fs.fileType == 0x02 ? "Value"
                            : fs.fileType == 0x03 ? "Linear Record"
                            : fs.fileType == 0x04 ? "Cyclic Record"
                                                   : "Unbekannt";
      const char *commName = fs.commMode == 0x00 ? "Plain" : fs.commMode == 0x01 ? "MACed" : "Enciphered";
      if (haveLog) {
        log.printf("   Datei %u: Typ=%s, Kommunikation=%s", fileIds[f], typeName, commName);
        if (fs.fileType == 0x00 || fs.fileType == 0x01) log.printf(", Groesse=%u Byte", (unsigned)fs.fileSize);
        log.println();
      }

      if (fs.commMode == 0x03) {
        if (haveLog) log.println("     -> Enciphered, wird nicht gelesen (Entschluesselung nicht implementiert).");
        continue;
      }

      uint8_t dataBuf[128];
      uint16_t dataLen = 0;
      if (fs.fileType == 0x00 || fs.fileType == 0x01) {
        dataLen = desfireReadData(fileIds[f], dataBuf, sizeof(dataBuf));
      } else if (fs.fileType == 0x03 || fs.fileType == 0x04) {
        dataLen = desfireReadRecords(fileIds[f], dataBuf, sizeof(dataBuf));
      }
      if (dataLen > 0 && haveLog) {
        log.print("     Inhalt: ");
        for (uint16_t i = 0; i < dataLen; i++) log.printf("%02X ", dataBuf[i]);
        log.println();
        if (fs.commMode == 0x01) log.println("     (MACed -- MAC/CMAC wurde NICHT geprueft, nur Rohdaten gezeigt)");
      }
    }
  }

  if (haveLog) log.close();
  snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "DESFire: %u App(s), %u authentifiziert, %s",
           appCount, authedApps, sdMounted ? "siehe SD-Log" : "SD nicht gemountet");
}

// =======================================================================
// DESFire-Schreibfunktionen (die 7 neuen Buttons) -- auf Nutzerwunsch,
// siehe firmware/README.md fuer Umfang/Einschraenkungen/Risikohinweise.
// Jede Funktion aktiviert die Karte selbst (unabhaengig vom Auto-Scan-
// Polling oben) und piept erst am ENDE (Erfolg ODER Fehlschlag), nicht
// beim blossen Erkennen der Karte.
// =======================================================================

// Aktiviert eine aufliegende Karte fuer EIN einzelnes Schreibkommando.
// Meldet ueber desfireSummaryMsg + Rueckgabewert, falls keine Karte da ist.
bool desfireOpBegin() {
  if (!nfc.inListPassiveTarget()) {
    snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Keine Karte aufgelegt.");
    updateUidInfo();
    return false;
  }
  return true;
}

// Gemeinsamer Abschluss aller 7 Op-Funktionen: setzt den Piepton (kurz bei
// Erfolg, lang bei Fehlschlag) und den Auto-Scan-Entprellzeitpunkt.
// updateUidInfo() bleibt hier (zeichnet zwar nur auf den -- waehrend des
// DESFire-Aktions-Fensters unsichtbaren -- Hauptbildschirm, schadet aber
// nicht und sorgt dafuer, dass der Hauptbildschirm nach dem Schliessen des
// Fensters bereits den richtigen Stand hat). Das eigentliche Ergebnis
// zeigt loop() im Fenster selbst (drawDesfireModalResult()).
void desfireOpFinish(bool ok) {
  updateUidInfo();
  buzzerBeep(ok ? 80 : 300);
  lastUidReadMs = millis();
}

// "MASTER-PW SETZEN": authentifiziert mit dem AKTUELL gueltigen Schluessel
// (Default ODER Custom, je nachdem was gerade gilt -- siehe
// desfireAuthEitherKey()) und setzt Key 0 per ChangeKey auf CUSTOM_KEY.
// Betrifft SOWOHL die PICC-Ebene (AID 000000, die GANZE Karte) ALS AUCH,
// falls schon vorhanden, die eigene Applikation (CUSTOM_AID) -- beides in
// einem Tastendruck. Existiert die Applikation noch nicht, wird nur die
// PICC-Ebene gesichert; ein erneuter Tastendruck NACH "APP ERSTELLEN"
// sichert dann auch die App nachtraeglich.
void desfireOpSetMasterKey() {
  if (!desfireOpBegin()) return;
  bool anyOk = false;

  if (desfireSelectApplication(PICC_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      if (usedCustom) {
        logMsg("desfireOpSetMasterKey(): PICC-Key ist bereits der Custom-Key.");
        anyOk = true;
      } else if (desfireChangeKeySame(0, CUSTOM_KEY, sessionKey, iv, isAes)) {
        logMsg("desfireOpSetMasterKey(): PICC-Master-Key gesetzt.");
        anyOk = true;
      } else {
        logMsg("desfireOpSetMasterKey(): ChangeKey auf PICC-Ebene fehlgeschlagen.");
      }
    } else {
      logMsg("desfireOpSetMasterKey(): Authentifizierung auf PICC-Ebene fehlgeschlagen.");
    }
  }

  if (desfireSelectApplication(CUSTOM_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      if (usedCustom) {
        logMsg("desfireOpSetMasterKey(): App-Key ist bereits der Custom-Key.");
        anyOk = true;
      } else if (desfireChangeKeySame(0, CUSTOM_KEY, sessionKey, iv, isAes)) {
        logMsg("desfireOpSetMasterKey(): App-Key gesetzt.");
        anyOk = true;
      } else {
        logMsg("desfireOpSetMasterKey(): ChangeKey auf App-Ebene fehlgeschlagen.");
      }
    } else {
      logMsg("desfireOpSetMasterKey(): Auth auf App-Ebene fehlgeschlagen (App evtl. noch nicht angelegt).");
    }
  }

  snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg),
           anyOk ? "Master-PW gesetzt (siehe Serial)" : "Master-PW setzen fehlgeschlagen (siehe Serial)");
  desfireOpFinish(anyOk);
}

// "AUF STANDARD": Kehrbild zu oben -- authentifiziert mit dem aktuellen
// Schluessel; war es der Custom-Key, wird per ChangeKey auf den
// Werks-Default (16 Nullbytes) zurueckgesetzt. War es schon der Default,
// ist nichts zu tun. Ebenfalls PICC- UND App-Ebene in einem Tastendruck.
void desfireOpResetMasterKey() {
  if (!desfireOpBegin()) return;
  bool anyOk = false;
  static const uint8_t zeroKey[16] = {0};

  if (desfireSelectApplication(PICC_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      if (!usedCustom) {
        logMsg("desfireOpResetMasterKey(): PICC-Key ist bereits Standard.");
        anyOk = true;
      } else if (desfireChangeKeySame(0, zeroKey, sessionKey, iv, isAes)) {
        logMsg("desfireOpResetMasterKey(): PICC-Master-Key zurueckgesetzt.");
        anyOk = true;
      } else {
        logMsg("desfireOpResetMasterKey(): ChangeKey auf PICC-Ebene fehlgeschlagen.");
      }
    } else {
      logMsg("desfireOpResetMasterKey(): Authentifizierung auf PICC-Ebene fehlgeschlagen.");
    }
  }

  if (desfireSelectApplication(CUSTOM_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      if (!usedCustom) {
        logMsg("desfireOpResetMasterKey(): App-Key ist bereits Standard.");
        anyOk = true;
      } else if (desfireChangeKeySame(0, zeroKey, sessionKey, iv, isAes)) {
        logMsg("desfireOpResetMasterKey(): App-Key zurueckgesetzt.");
        anyOk = true;
      } else {
        logMsg("desfireOpResetMasterKey(): ChangeKey auf App-Ebene fehlgeschlagen.");
      }
    } else {
      logMsg("desfireOpResetMasterKey(): Auth auf App-Ebene fehlgeschlagen (App evtl. nicht vorhanden).");
    }
  }

  snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg),
           anyOk ? "Auf Standard zurueckgesetzt (siehe Serial)" : "Zuruecksetzen fehlgeschlagen (siehe Serial)");
  desfireOpFinish(anyOk);
}

// "APP ERSTELLEN": PICC-Ebene auswaehlen+authentifizieren, CUSTOM_AID
// anlegen, dann in die neue App wechseln (frisch angelegt = Werks-
// Default-Schluessel) und eine Value-Datei (FileNo 0, Guthaben startet
// bei 0) darin erstellen. Cipher fuer die neue App richtet sich danach,
// mit welchem Cipher die PICC-Ebene authentifiziert wurde (2K3DES oder AES).
void desfireOpCreateApp() {
  if (!desfireOpBegin()) return;
  bool ok = false;

  if (desfireSelectApplication(PICC_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      if (desfireCreateApplication(CUSTOM_AID, isAes)) {
        if (desfireSelectApplication(CUSTOM_AID)) {
          uint8_t sessionKey2[16], iv2[16];
          const char *cipherName2; bool isAes2, usedCustom2;
          if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey2, iv2, &cipherName2, &isAes2, &usedCustom2)) {
            ok = desfireCreateValueFile(VALUE_FILE_NO, VALUE_LOWER_LIMIT, VALUE_UPPER_LIMIT, 0);
          } else {
            logMsg("desfireOpCreateApp(): Auth in neuer App fehlgeschlagen.");
          }
        }
      } else {
        logMsg("desfireOpCreateApp(): CreateApplication fehlgeschlagen (existiert sie schon?).");
      }
    } else {
      logMsg("desfireOpCreateApp(): Authentifizierung auf PICC-Ebene fehlgeschlagen.");
    }
  }

  snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg),
           ok ? "Applikation erstellt (Guthaben 0)" : "Applikation erstellen fehlgeschlagen");
  desfireOpFinish(ok);
}

// "APP LOESCHEN": muss auf PICC-Ebene authentifiziert aufgerufen werden
// (DESFire-Vorgabe, nicht innerhalb der zu loeschenden App).
void desfireOpDeleteApp() {
  if (!desfireOpBegin()) return;
  bool ok = false;

  if (desfireSelectApplication(PICC_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      ok = desfireDeleteApplication(CUSTOM_AID);
    } else {
      logMsg("desfireOpDeleteApp(): Authentifizierung auf PICC-Ebene fehlgeschlagen.");
    }
  }

  snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg),
           ok ? "Applikation geloescht" : "Applikation loeschen fehlgeschlagen");
  desfireOpFinish(ok);
}

// "GUTHABEN BUCHEN": Credit + CommitTransaction (ohne Commit wird die
// Buchung beim naechsten Kommando/Verlassen des Feldes verworfen).
void desfireOpCredit() {
  if (!desfireOpBegin()) return;
  bool ok = false;

  if (desfireSelectApplication(CUSTOM_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      ok = desfireCredit(VALUE_FILE_NO, CREDIT_DEBIT_AMOUNT) && desfireCommitTransaction();
    } else {
      logMsg("desfireOpCredit(): Authentifizierung fehlgeschlagen (App evtl. nicht vorhanden).");
    }
  }

  if (ok) snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Guthaben gebucht (+%ld)", (long)CREDIT_DEBIT_AMOUNT);
  else    snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Guthaben buchen fehlgeschlagen");
  desfireOpFinish(ok);
}

// "GUTHABEN NUTZEN": Debit + CommitTransaction. Die KARTE SELBST lehnt
// das Debit ab (typischerweise Status BOUNDARY_ERROR), wenn das Guthaben
// dadurch unter VALUE_LOWER_LIMIT (0) fallen wuerde -- keine eigene
// Vorab-Pruefung noetig.
void desfireOpDebit() {
  if (!desfireOpBegin()) return;
  bool ok = false;
  bool debitRejected = false;

  if (desfireSelectApplication(CUSTOM_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      if (desfireDebit(VALUE_FILE_NO, CREDIT_DEBIT_AMOUNT)) {
        ok = desfireCommitTransaction();
      } else {
        debitRejected = true;
      }
    } else {
      logMsg("desfireOpDebit(): Authentifizierung fehlgeschlagen (App evtl. nicht vorhanden).");
    }
  }

  if (ok) snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Guthaben genutzt (-%ld)", (long)CREDIT_DEBIT_AMOUNT);
  else if (debitRejected) snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Zu wenig Guthaben (Debit abgelehnt)");
  else snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Guthaben nutzen fehlgeschlagen");
  desfireOpFinish(ok);
}

// "GUTHABEN ABFRAGEN": GetValue, Ergebnis landet direkt in der
// DESFire-Zusammenfassungszeile.
void desfireOpGetValue() {
  if (!desfireOpBegin()) return;
  bool ok = false;
  int32_t value = 0;

  if (desfireSelectApplication(CUSTOM_AID)) {
    uint8_t sessionKey[16], iv[16];
    const char *cipherName; bool isAes, usedCustom;
    if (desfireAuthEitherKey(0, CUSTOM_KEY, sessionKey, iv, &cipherName, &isAes, &usedCustom)) {
      ok = desfireGetValue(VALUE_FILE_NO, value);
    } else {
      logMsg("desfireOpGetValue(): Authentifizierung fehlgeschlagen (App evtl. nicht vorhanden).");
    }
  }

  if (ok) snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Guthaben: %ld", (long)value);
  else    snprintf(desfireSummaryMsg, sizeof(desfireSummaryMsg), "Guthaben abfragen fehlgeschlagen");
  desfireOpFinish(ok);
}

// Fuehrt die zum aktuell gewaehlten Fenster gehoerende DESFire-Aktion aus
// (siehe DesfireModalState/loop()) -- muss NACH den desfireOpXxx()-
// Funktionen stehen: anders als beim .ino-Build (Arduino-IDE, automatische
// Funktions-Prototypen) generiert PlatformIOs main.cpp-Build KEINE
// Prototypen, Vorwaertsreferenzen auf spaeter im File definierte
// Funktionen wuerden dort nicht kompilieren.
void desfireRunModalAction(DesfireAction action) {
  switch (action) {
    case DESFIRE_ACTION_SET_MASTER_KEY:   desfireOpSetMasterKey(); break;
    case DESFIRE_ACTION_RESET_MASTER_KEY: desfireOpResetMasterKey(); break;
    case DESFIRE_ACTION_CREATE_APP:       desfireOpCreateApp(); break;
    case DESFIRE_ACTION_DELETE_APP:       desfireOpDeleteApp(); break;
    case DESFIRE_ACTION_CREDIT:           desfireOpCredit(); break;
    case DESFIRE_ACTION_DEBIT:            desfireOpDebit(); break;
    case DESFIRE_ACTION_GET_VALUE:        desfireOpGetValue(); break;
    default: break;
  }
}

// Core-Versions-abhaengig (siehe README.md Abschnitt 15, Punkt 7).
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
  haveSendResult = true;
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(espnow_message_t)) return;
  memcpy(&lastReceived, data, sizeof(espnow_message_t));
  memcpy(lastSrcMac, info->src_addr, 6);
  haveReceived = true;
  recvPending = true;
}

#ifdef USE_WIFI_NTP_SYNC
// Gibt true/false zurueck, statt nur nach Serial zu loggen -- der
// Touch-Handler (siehe loop()) laesst den NTP-SYNC-Button dafuer kurz rot
// aufleuchten, wenn es NICHT geklappt hat.
//
// Verbindet SELBST NICHT mehr mit dem AP -- das macht ausschliesslich der
// WLAN/ESP-NOW-Umschalt-Button (siehe loop()). Vorher rief diese Funktion
// bei jedem NTP-SYNC-Tastendruck WiFi.begin() neu auf und wartete bis zu
// 10s darauf, was ESP-NOW zwischendurch unterbrach; jetzt wird nur noch
// geprueft, ob (dank des Umschalt-Buttons) BEREITS eine WLAN-Verbindung
// besteht.
bool syncFromNtp() {
  bool haveSerial = (bool)Serial;
  if (WiFi.status() != WL_CONNECTED) {
    if (haveSerial) logMsg("syncFromNtp(): WLAN nicht verbunden -- erst WLAN/ESP-NOW-Button auf WLAN stellen.");
    return false;
  }

  // EU-Zeitzonenregel (Luxemburg) inkl. automatischer Sommerzeitumstellung.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "europe.pool.ntp.org");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    if (haveSerial) logMsg("NTP-Sync fehlgeschlagen (Timeout -- Server nicht erreichbar?).");
    return false;
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
    if (haveSerial) logMsg("RTC per NTP synchronisiert.");
    return true;
  }
  if (haveSerial) logMsg("FEHLER: RTC-Schreibzugriff nach NTP-Sync fehlgeschlagen.");
  return false;
}
#endif

void setup() {
  // Muss vor allem anderen laufen (siehe touch_probe.h) -- zwingt den GT911
  // deterministisch auf Adresse 0x5D, statt den Pegel floaten zu lassen.
  gt911PowerOnSequence();

  Serial.begin(115200);
  delay(200);
  logMsg("Stage 5: PN532 NFC + SD + RTC + WLAN/ESP-NOW -- boot");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
  delay(50);

  // Zusaetzlich zur 120ms-Adress-Auswahl-Sequenz (gt911PowerOnSequence())
  // braucht der GT911-Chip selbst nach einem ECHTEN Stromlos-Start noch
  // Zeit fuer sein eigenes Power-On-Booting (siehe firmware/README.md).
  delay(600);

  uint8_t gtAddr = probeGT911Address();
  touchStandaloneConfig(gtAddr);
  logMsg("Touch-Init -> %s\n", touchStandaloneInit() ? "OK" : "FEHLGESCHLAGEN");

  logMsg("rgbPanelInit() (esp_lcd_panel_rgb, mit Bounce Buffer) ...");
  if (!rgbPanelInit()) {
    logMsg("Panel-Init fehlgeschlagen -- Sketch haelt an.");
    while (true) { delay(1000); }
  }
  canvas.setColorDepth(16);
  canvas.setBuffer(g_rgbFrameBuffer, LCD_WIDTH, LCD_HEIGHT, 16);
  // 90-Grad-Drehung (siehe DISPLAY_ROTATION oben) -- rein eine
  // Koordinatentransformation von LovyanGFX auf demselben physischen
  // Framebuffer, die RGB-Timings in rgb_panel.h bleiben unveraendert bei
  // 800x480. canvas.width()/height() liefern ab hier 480/800.
  canvas.setRotation(DISPLAY_ROTATION);

  setBacklightPercent(backlightPercent);

  WiFi.mode(WIFI_STA);

#ifdef USE_WIFI_NTP_SYNC
  // Alle in wifi_secrets.h eingetragenen Netzwerke bei WiFiMulti anmelden --
  // muss nur einmal passieren, das eigentliche Verbinden/Auswaehlen
  // uebernimmt spaeter wifiMulti.run() (siehe WLAN/ESP-NOW-Umschalter und
  // das periodische Reconnect-Polling in loop()).
  for (size_t i = 0; i < sizeof(WIFI_SECRETS) / sizeof(WIFI_SECRETS[0]); i++) {
    wifiMulti.addAP(WIFI_SECRETS[i].ssid, WIFI_SECRETS[i].password);
    logMsg("WiFiMulti: Netzwerk '%s' registriert.", WIFI_SECRETS[i].ssid);
  }
#endif

  // Direkt nach WiFi.mode() liefert WiFi.macAddress() auf manchen Boards bei
  // echtem Stromlos-Start noch "00:00:00:00:00:00" -- kurz auf eine echte
  // Adresse warten (siehe firmware/README.md Abschnitt 7).
  {
    uint32_t macWaitStart = millis();
    while (WiFi.macAddress() == "00:00:00:00:00:00" && millis() - macWaitStart < 2000) {
      delay(50);
    }
  }

  if (esp_now_init() != ESP_OK) {
    logMsg("FEHLER: esp_now_init() fehlgeschlagen");
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddr, 6);
  peerInfo.channel = 0;   // aktueller WiFi-Kanal
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    logMsg("FEHLER: esp_now_add_peer() fehlgeschlagen");
  }

  outgoing.counter = 0;
  snprintf(outgoing.text, sizeof(outgoing.text), "Hallo von %s", WiFi.macAddress().c_str());

  RtcDateTime dt;
  if (rtcRead(dt) && dt.voltageLow) {
    logMsg("Hinweis: VL-Bit gesetzt -- Zeit nach Spannungsausfall unsicher, bitte stellen.");
  }

  checkSdCard();
  pn532Ready = pn532Begin();
  if (!pn532Ready) {
    buzzerBeep(300);
  }

  redrawMainScreen();
  displayReady = true;
  logMsg("Setup fertig.");
}

uint32_t lastRtcDrawMs = 0;
uint32_t lastSdPollMs = 0;

void loop() {
  uint32_t now = millis();

  // Der komplette Hintergrund-Block (Uhrzeit, SD-Polling, WLAN-Polling,
  // ESP-NOW-Senden, automatischer Lese-Scan) pausiert KOMPLETT, solange
  // ein DESFire-Aktions-Fenster offen ist (siehe DesfireModalState) --
  // sonst wuerde jeder dieser periodischen Redraws Teile des
  // Hauptbildschirms unter dem Fenster neu zeichnen, was durch das
  // Fenster "durchscheinen" wuerde (alles ein einziger flacher
  // Framebuffer ohne echte Fenster-Ueberlagerung/Z-Ordnung). Der
  // automatische Lese-Scan war zusaetzlich vorher die Ursache dafuer,
  // dass ein Tastendruck-Ergebnis fast sofort wieder ueberschrieben
  // wurde -- mit eigenem Fenster ist das jetzt strukturell ausgeschlossen,
  // nicht mehr nur durch eine Wartezeit kaschiert.
  if (desfireModalState == MODAL_CLOSED && !settingsOpen && !logViewOpen) {
    // Nur hier, im loop()-Task, wird tatsaechlich gezeichnet/geloggt -- siehe
    // Kommentar bei recvPending oben.
    if (recvPending) {
      recvPending = false;
      updateReceivedInfo();
      logReceivedMessage();
    }

    if (now - lastRtcDrawMs >= RTC_UPDATE_INTERVAL_MS) {
      lastRtcDrawMs = now;
      updateRtcInfo();
    }

    if (now - lastSdPollMs >= SD_POLL_INTERVAL_MS) {
      lastSdPollMs = now;
      checkSdCard();
    }

    // WiFi.begin() (siehe WLAN/ESP-NOW-Umschalter) verbindet asynchron im
    // Hintergrund -- ohne dieses Polling wuerde die WLAN-IP-Zeile erst nach
    // dem naechsten Tastendruck aktualisiert, obwohl die Verbindung evtl.
    // laengst steht. Nur aktiv, waehrend der WLAN-Modus eingeschaltet ist.
    if (wlanModeActive && now - lastWifiPollMs >= WIFI_POLL_INTERVAL_MS) {
      lastWifiPollMs = now;
      updateWifiInfo();
    }

#ifdef USE_WIFI_NTP_SYNC
    // Reconnect, falls die Verbindung zwischenzeitlich wegbrach (z. B.
    // Netzwerk kurz ausser Reichweite) oder das staerkste Netzwerk
    // gewechselt hat -- deutlich seltener als das reine Status-Polling
    // oben, da hier bei fehlender Verbindung ein blockierender Scan
    // anfaellt (siehe Kommentar bei WIFI_MULTI_RETRY_INTERVAL_MS).
    if (wlanModeActive && WiFi.status() != WL_CONNECTED &&
        now - lastWifiMultiRetryMs >= WIFI_MULTI_RETRY_INTERVAL_MS) {
      lastWifiMultiRetryMs = now;
      wifiMulti.run();
      logWifiAttemptResult();
      updateWifiInfo();
    }
#endif

    if (now - lastSendMs >= SEND_INTERVAL_MS) {
      lastSendMs = now;
      outgoing.counter++;
      esp_now_send(broadcastAddr, (uint8_t *)&outgoing, sizeof(outgoing));
      updateSendInfo();
    }

    // Poll-Versuch, danach 1s Entprellen, damit eine aufliegende Karte nicht
    // staendig neu "erkannt" wird. NUR EIN Aufruf von inListPassiveTarget()
    // fuer Erkennung UND Aktivierung -- ein zusaetzlicher
    // readPassiveTargetID()-Aufruf davor (frueherer Ansatz) sendet dasselbe
    // native InListPassiveTarget-Kommando ein zweites Mal an eine schon
    // aktivierte Karte und schlug an echter Hardware regelmaessig fehl
    // ("DESFire: inListPassiveTarget() fehlgeschlagen" trotz zuvor
    // erfolgreich gelesener UID -- siehe Kopfkommentar der Datei).
    //
    // WICHTIG: Diese Bibliotheksversion (adafruit/Adafruit PN532@^1.3.3)
    // bietet inListPassiveTarget() NUR parameterlos an -- keine Ueberladung
    // mit Timeout-Argument (ein frueherer Versuch, hier explizit ein 100ms-
    // Timeout zu uebergeben, ist am echten Build mit "no matching function"
    // gescheitert). Das eigentliche Problem liegt ohnehin nicht am Software-
    // Timeout dieses Aufrufs, sondern daran, wie oft der PN532-CHIP SELBST
    // intern nach einer Karte sucht, bevor er ueberhaupt antwortet -- siehe
    // nfc.setPassiveActivationRetries(1) in pn532Begin().
    if (pn532Ready && now - lastPn532AttemptMs >= PN532_POLL_INTERVAL_MS) {
      lastPn532AttemptMs = now;
      if (now - lastUidReadMs >= PN532_DEBOUNCE_MS) {
        if (nfc.inListPassiveTarget()) {
          desfireDeepRead(); // setzt uidMsg + desfireSummaryMsg
          updateUidInfo();
          // Erst piepen, wenn der komplette Lesevorgang abgeschlossen ist --
          // vorher piepte es sofort bei Kartenerkennung, noch bevor ueberhaupt
          // feststand, ob/was gelesen werden konnte.
          buzzerBeep(80);
          lastUidReadMs = now;
        }
      }
    }
  }

  int32_t x, y;
  if (touchStandaloneGetXY(&x, &y)) {
    // Rotations-Debug-Ausgabe (roh vs. gedreht) entfernt -- Rotation ist
    // an echter Hardware bestaetigt korrekt (siehe Fotos), und die Zeile
    // haette bei jedem Touch-Poll das Log-Fenster geflutet.
    rotateTouchToLogical(x, y);

    // Log-Fenster: eigener Zustand wie das DESFire-Fenster/Einstellungen
    // -- solange offen, werden AUSSCHLIESSLICH dessen eigene Buttons
    // ausgewertet.
    if (logViewOpen) {
      if (inside(btnLogViewUp, x, y)) {
        int maxOffset = logCount > LOGVIEW_VISIBLE_LINES ? logCount - LOGVIEW_VISIBLE_LINES : 0;
        if (logScrollOffset < maxOffset) logScrollOffset++;
        drawLogView();
      } else if (inside(btnLogViewDown, x, y)) {
        if (logScrollOffset > 0) logScrollOffset--;
        drawLogView();
      } else if (inside(btnLogViewClose, x, y)) {
        buzzerBeep(80);
        logViewOpen = false;
        redrawMainScreen();
      }
      delay(150);
      return;
    }

    // Solange das DESFire-Aktions-Fenster offen ist, werden AUSSCHLIESSLICH
    // dessen eigene Buttons ausgewertet -- die Buttons des Hauptbildschirms
    // darunter sind inaktiv (sie sind ja auch unsichtbar).
    if (desfireModalState == MODAL_CONFIRM) {
      if (inside(btnModalCancel, x, y)) {
        buzzerBeep(80);
        desfireModalState = MODAL_CLOSED;
        redrawMainScreen();
      } else if (inside(btnModalConfirm, x, y)) {
        drawButton(btnModalConfirm, TFT_GREEN);
        desfireRunModalAction(desfireModalAction); // setzt desfireSummaryMsg, piept via desfireOpFinish()
        desfireModalState = MODAL_RESULT;
        drawDesfireModalResult();
      }
      delay(150);
      return;
    } else if (desfireModalState == MODAL_RESULT) {
      if (inside(btnModalClose, x, y)) {
        buzzerBeep(80);
        desfireModalState = MODAL_CLOSED;
        redrawMainScreen();
      }
      delay(150);
      return;
    }

    // EINSTELLUNGEN-Untermenue: eigene Buttons, kein Bestaetigungsschritt
    // (siehe Deklaration von settingsOpen oben).
    if (settingsOpen) {
      if (inside(btnSettingsNtpSync, x, y)) {
        drawButton(btnSettingsNtpSync, TFT_GREEN);
        bool ntpOk = false;
#ifdef USE_WIFI_NTP_SYNC
        ntpOk = syncFromNtp();
        updateRtcInfo();
#else
        logMsg("NTP-Sync deaktiviert -- wifi_secrets.h fehlt (siehe wifi_secrets.example.h) oder es wurde seit dem Anlegen nicht neu kompiliert.");
#endif
        buzzerBeep(80);
        drawButton(btnSettingsNtpSync, ntpOk ? TFT_DARKGREY : TFT_RED);
        if (!ntpOk) {
          delay(500);
          drawButton(btnSettingsNtpSync, TFT_DARKGREY);
        }
      } else if (inside(btnSettingsWifiToggle, x, y)) {
        drawButton(btnSettingsWifiToggle, TFT_GREEN);
#ifdef USE_WIFI_NTP_SYNC
        wlanModeActive = !wlanModeActive;
        if (wlanModeActive) {
          // wifiMulti.run() blockiert bis zu 5s (Scan + Verbindungsversuch
          // mit dem staerksten bekannten Netzwerk) -- akzeptabel, da dies
          // nur bei einem bewussten Tastendruck passiert, nicht periodisch
          // (siehe periodisches Reconnect-Polling unten, das deutlich
          // seltener und mit kuerzerem Timeout laeuft).
          wifiMulti.run();
          lastWifiMultiRetryMs = millis();
          logMsg("WLAN-Modus aktiviert -- verbinde mit staerkstem bekannten Netzwerk (siehe WLAN-IP-Zeile) ...");
          logWifiAttemptResult();
        } else {
          WiFi.disconnect();
          logMsg("ESP-NOW-Modus aktiviert -- WLAN-Verbindung getrennt.");
        }
        updateWifiInfo();
#else
        logMsg("WLAN/ESP-NOW-Umschaltung deaktiviert -- wifi_secrets.h fehlt (siehe wifi_secrets.example.h) oder es wurde seit dem Anlegen nicht neu kompiliert.");
#endif
        buzzerBeep(80);
        drawButton(btnSettingsWifiToggle, TFT_DARKGREY);
      } else if (inside(btnSettingsBrightUp, x, y)) {
        backlightPercent = (backlightPercent <= 90) ? backlightPercent + 10 : 100;
        setBacklightPercent(backlightPercent);
        drawButton(btnSettingsBrightUp, TFT_GREEN);
        delay(80);
        drawButton(btnSettingsBrightUp, TFT_DARKGREY);
      } else if (inside(btnSettingsBrightDn, x, y)) {
        backlightPercent = (backlightPercent >= 10) ? backlightPercent - 10 : 0;
        setBacklightPercent(backlightPercent);
        drawButton(btnSettingsBrightDn, TFT_GREEN);
        delay(80);
        drawButton(btnSettingsBrightDn, TFT_DARKGREY);
      } else if (inside(btnModalClose, x, y)) {
        buzzerBeep(80);
        settingsOpen = false;
        redrawMainScreen();
      }
      delay(150);
      return;
    }

    if (inside(btnSettingsOpen, x, y)) {
      buzzerBeep(80);
      settingsOpen = true;
      drawSettingsMenu();
    } else if (inside(btnLogOpen, x, y)) {
      buzzerBeep(80);
      logScrollOffset = 0; // beim Oeffnen immer die neuesten Zeilen zeigen
      logViewOpen = true;
      drawLogView();
    } else if (inside(btnSetMasterKey, x, y)) {
      buzzerBeep(80);
      desfireModalAction = DESFIRE_ACTION_SET_MASTER_KEY;
      desfireModalState = MODAL_CONFIRM;
      drawDesfireModalConfirm();
    } else if (inside(btnResetMasterKey, x, y)) {
      buzzerBeep(80);
      desfireModalAction = DESFIRE_ACTION_RESET_MASTER_KEY;
      desfireModalState = MODAL_CONFIRM;
      drawDesfireModalConfirm();
    } else if (inside(btnCreateApp, x, y)) {
      buzzerBeep(80);
      desfireModalAction = DESFIRE_ACTION_CREATE_APP;
      desfireModalState = MODAL_CONFIRM;
      drawDesfireModalConfirm();
    } else if (inside(btnDeleteApp, x, y)) {
      buzzerBeep(80);
      desfireModalAction = DESFIRE_ACTION_DELETE_APP;
      desfireModalState = MODAL_CONFIRM;
      drawDesfireModalConfirm();
    } else if (inside(btnCredit, x, y)) {
      buzzerBeep(80);
      desfireModalAction = DESFIRE_ACTION_CREDIT;
      desfireModalState = MODAL_CONFIRM;
      drawDesfireModalConfirm();
    } else if (inside(btnDebit, x, y)) {
      buzzerBeep(80);
      desfireModalAction = DESFIRE_ACTION_DEBIT;
      desfireModalState = MODAL_CONFIRM;
      drawDesfireModalConfirm();
    } else if (inside(btnGetValue, x, y)) {
      buzzerBeep(80);
      desfireModalAction = DESFIRE_ACTION_GET_VALUE;
      desfireModalState = MODAL_CONFIRM;
      drawDesfireModalConfirm();
    } else if (y < BTN_ROW1_Y) {
      // Tap auf freie Flaeche -> Hintergrundfarbe wechseln (Panel-Refresh-Test)
      bgIndex = (bgIndex + 1) % (sizeof(bgColors) / sizeof(bgColors[0]));
      redrawMainScreen();
    }

    delay(150); // einfaches Debounce
  }
}
