#pragma once
// Kopiere diese Datei nach "wifi_secrets.h" (wird per .gitignore ignoriert)
// und trage die eigenen Zugangsdaten ein. Nur noetig, wenn zusaetzlich zu
// ESP-NOW auch eine echte WLAN-Verbindung (z. B. fuer spaetere NTP-Zeitsync,
// siehe Stage 3) aufgebaut werden soll -- fuer reines ESP-NOW nicht
// erforderlich, ESP-NOW funktioniert auch ohne AP-Verbindung.

#define WIFI_SSID     "dein-wlan-name"
#define WIFI_PASSWORD "dein-wlan-passwort"
