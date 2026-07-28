#pragma once
// Kopiere diese Datei nach "wifi_secrets.h" (wird per .gitignore ignoriert)
// und trage die eigenen Zugangsdaten ein. Nur noetig, wenn zusaetzlich zu
// ESP-NOW auch eine echte WLAN-Verbindung (z. B. fuer NTP-Zeitsync)
// aufgebaut werden soll -- fuer reines ESP-NOW nicht erforderlich, ESP-NOW
// funktioniert auch ohne AP-Verbindung.
//
// Beliebig viele Netzwerke eintragen (z. B. Zuhause + Werkstatt + Handy-
// Hotspot) -- das Board verbindet sich beim Einschalten des WLAN-Modus
// automatisch mit dem staerksten Netzwerk aus dieser Liste, das gerade in
// Reichweite ist (ueber die ESP32-Arduino-Core-Klasse WiFiMulti). Mindestens
// ein Eintrag ist noetig.

struct WifiSecretEntry {
  const char *ssid;
  const char *password;
};

static const WifiSecretEntry WIFI_SECRETS[] = {
  { "dein-wlan-name-1", "dein-wlan-passwort-1" },
  { "dein-wlan-name-2", "dein-wlan-passwort-2" },
  // weitere Zeilen nach demselben Muster ergaenzen ...
};
