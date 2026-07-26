# Abschlusstest — DESFire-Terminal, CrowPanel Advance 5.0

Ziel: einmaliger, durchgängiger Test aller Komponenten in Betriebsreihenfolge, bevor das Terminal in die Integration mit dem Django-Backend geht. Erst durchführen, wenn das offene Display-Problem (siehe Projektdokumentation, Abschnitt 9) gelöst ist — ein Test auf instabiler Basis liefert keine verlässliche Aussage.

---

## Voraussetzungen vor Testbeginn

- [ ] Display zeigt bei **fünf aufeinanderfolgenden Kaltstarts** (USB komplett abgezogen, nicht nur Reset-Taste) zuverlässig ein Bild
- [ ] Touch reagiert bei allen fünf Kaltstarts ohne Nacharbeit
- [ ] Board-Einstellungen kontrolliert: PSRAM=OPI, Flash=16MB, Partition=Huge APP, Core=3.3.11, LovyanGFX=1.2.26
- [ ] `pins.h`: `DF_CUSTOM_KEY_BYTES` auf einen echten, dokumentierten Wert gesetzt (nicht der Platzhalter) und sicher notiert
- [ ] Eine Testkarte vorhanden, die notfalls dauerhaft gesperrt werden darf (für die Master-Key-Tests)
- [ ] Eine zweite, unkritische Karte für die reinen Lese-/Guthaben-Tests

---

## Testreihenfolge

### 1. Kaltstart-Stabilität

- [ ] Strom vollständig trennen, 10 Sekunden warten, wieder anschließen
- [ ] Display zeigt Startsequenz (Init, WiFi, PN532)
- [ ] Kein Flackern, kein Bildabriss über mindestens 60 Sekunden Standbetrieb
- [ ] Wiederholen: **3 weitere Kaltstarts**, jeweils Ergebnis notieren

### 2. WiFi + Zeit

- [ ] WiFi verbindet innerhalb von 15 Sekunden, IP wird angezeigt
- [ ] NTP-Sync erfolgreich, Uhrzeit auf dem Display korrekt (Vergleich mit Handy-Uhrzeit)
- [ ] Strom trennen, 2 Minuten warten, wieder anschließen — Uhrzeit läuft aus der RTC korrekt weiter (Pufferbatterie-Test)

### 3. Touch

- [ ] Alle 7 Buttons einzeln antippen, jeder reagiert mit sichtbarer Rückmeldung
- [ ] Koordinaten stimmen mit sichtbarer Buttonposition überein (kein Versatz)
- [ ] Buzzer-Piep kommt bei jeder abgeschlossenen Aktion, nicht beim Start

### 4. PN532 / Kartenerkennung

- [ ] Testkarte auflegen, UID wird korrekt angezeigt (7 Byte, `04 A4 AB 4A F6 21 90` oder aktuelle Testkarte)
- [ ] GetVersion liefert korrekte Werte: NXP, EV3, Speichergröße
- [ ] Karte mehrfach auflegen/entfernen, Erkennung bleibt zuverlässig (kein Aussetzen nach mehrmaliger Nutzung)

### 5. DESFire-Lebenszyklus — unkritische Karte zuerst

- [ ] **App erstellen**: Applikation + Value-File werden angelegt, Startwert 0
- [ ] **Abfragen**: Guthaben wird korrekt mit 0 angezeigt
- [ ] **Buchen**: Guthaben wird um den Testbetrag erhöht, Commit erfolgreich
- [ ] **Abfragen**: neuer Stand korrekt
- [ ] **Nutzen**: Guthaben wird reduziert, Commit erfolgreich
- [ ] **Abfragen**: neuer Stand korrekt
- [ ] **Nutzen** mit einem Betrag größer als das Guthaben: Operation wird korrekt abgelehnt (kein Absturz, klare Fehlermeldung)
- [ ] **App löschen**: Applikation ist danach nicht mehr auswählbar

### 6. Master-Key-Wechsel — nur mit der opferbaren Testkarte

- [ ] **Master PW setzen**: Auth mit Werksdefault gelingt, ChangeKey erfolgreich
- [ ] Karte danach mit Werksdefault-Key testen: Auth schlägt erwartungsgemäß fehl (Beweis, dass der Schlüssel wirklich geändert wurde)
- [ ] **Auf Standard setzen**: Auth mit Custom-Key gelingt, ChangeKey zurück auf Default erfolgreich
- [ ] Karte danach mit Werksdefault-Key erneut testen: Auth gelingt wieder (Beweis, dass der Rücksetzvorgang funktioniert)
- [ ] Custom-Key-Wert aus `pins.h` mit der schriftlichen Notiz abgleichen — müssen exakt übereinstimmen

### 7. Dauerlast / Wiederholbarkeit

- [ ] Denselben Zyklus (App erstellen → Buchen → Nutzen → Abfragen → Löschen) **10 Mal hintereinander** ohne manuelles Eingreifen zwischen den Durchläufen durchführen
- [ ] Keine Speicherlecks/Abstürze über die 10 Zyklen (Board bleibt responsiv, kein Reset nötig)
- [ ] SD-Karte (falls parallel getestet) bleibt währenddessen ansprechbar

### 8. Randfälle

- [ ] Karte während einer laufenden Operation wegnehmen (z. B. mitten in "Buchen") — Terminal zeigt klaren Fehler, kein Hänger
- [ ] Zwei verschiedene Karten kurz hintereinander aufwechseln — jede wird korrekt individuell erkannt
- [ ] Falsche/fremde Karte auflegen (z. B. Mifare Classic) — wird korrekt als "keine DESFire" erkannt, kein Absturz

---

## Nicht Teil dieses Abschlusstests (spätere Schritte)

- CMAC-/verschlüsselte Kommunikation nach Authentifizierung (aktuell Plain-Mode, siehe Projektdokumentation Abschnitt 12.1)
- Anbindung an das Django-Backend (WebSocket/HTTP)
- Mehrbenutzerbetrieb / mehrere Terminals gleichzeitig (ESP-NOW-Kanalabstimmung)
- Belastungstest unter realen Festbedingungen (viele Nutzer, Warteschlange, Dauerbetrieb über Stunden)

---

## Ergebnis-Protokoll

| Testabschnitt | Bestanden | Datum | Anmerkung |
|---|---|---|---|
| 1. Kaltstart-Stabilität | | | |
| 2. WiFi + Zeit | | | |
| 3. Touch | | | |
| 4. PN532/Kartenerkennung | | | |
| 5. DESFire-Lebenszyklus | | | |
| 6. Master-Key-Wechsel | | | |
| 7. Dauerlast | | | |
| 8. Randfälle | | | |

**Freigabe für Backend-Integration erst nach vollständigem Bestehen aller Abschnitte.**
