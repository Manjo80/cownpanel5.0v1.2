#pragma once
// Native DESFire-Kommandos ueber den PN532 (kein ISO7816-Wrapping -- reine
// Rohkommandos via Adafruit_PN532::inDataExchange(), das die aktuell per
// inListPassiveTarget() aktivierte Karte anspricht).
//
// AUSDRUECKLICH BEGRENZTER UMFANG (bewusste Entscheidung, siehe
// firmware/README.md):
//  - Unterstuetzt 2K3DES- (Kommando 0x1A, AUTHENTICATE_ISO -- siehe
//    Kommentar bei desfireAuthDes3(), warum NICHT das "klassische"
//    Legacy-Kommando 0x0A) und AES-128- (Kommando 0xAA) Authentifizierung
//    mit einem 16-Byte-Schluessel (Default-Schluessel = 16 Nullbytes).
//    KEIN 3K3DES, KEIN EV2-Secure-Messaging.
//  - Session-Key wird aus RndA/RndB abgeleitet und NUR fuer ChangeKey
//    genutzt (siehe desfireChangeKeySame()) -- KEIN CMAC, KEIN
//    Enciphered-Datenverkehr fuer Lese-/Schreib-Dateizugriffe. Dateien im
//    Enciphered-Modus werden ERKANNT, aber nicht entschluesselt (siehe
//    desfireGetFileSettings()/commMode). MACed-Dateien werden gelesen,
//    ihr MAC/CMAC wird NICHT geprueft (Daten selbst sind bei MACed-Modus
//    ohnehin unverschluesselt uebertragen, nur zusaetzlich signiert).
//    CreateValueFile() legt Value-Dateien deshalb immer im Plain-Modus an.
//  - Schreibende Kommandos (ChangeKey, CreateApplication,
//    DeleteApplication, CreateValueFile, Credit, Debit, CommitTransaction)
//    sind implementiert, ABER: ChangeKey nur fuer den Fall "eigenen,
//    gerade authentifizierten Schluessel aendern" (kein Schluesselwechsel
//    fuer einen ANDEREN als den authentifizierten Key). Siehe
//    Sicherheitshinweis bei desfireChangeKeySame().
//  - Dieser Code wurde NICHT an echter DESFire-Hardware getestet (kein
//    Testgeraet verfuegbar) -- er basiert auf der oeffentlich
//    dokumentierten NXP-Spezifikation (ISO/IEC 9798-2 3-Pass-Mutual-
//    Authentication, wie sie MIFARE DESFire (EV1) verwendet). Schlaegt
//    die Authentifizierung fehl, ist die wahrscheinlichste Erklaerung,
//    dass die Karte (wie bei den meisten echten Karten aus dem Feld)
//    NICHT mehr die Werksschluessel hat -- das ist dann korrektes
//    Verhalten, kein Bug. Serial-Ausgaben an jedem Protokollschritt
//    sollen helfen, einen echten Bug von "Karte hat andere Schluessel"
//    zu unterscheiden.
//  - DES/2K3DES ist HIER SELBST implementiert (Standard-FIPS-46-3-
//    Tabellen), NICHT ueber mbedtls: ESP-IDFs mbedtls-Komponente wird bei
//    framework=arduino als fertig vorkompiliertes Binary ausgeliefert
//    (nicht aus Quellcode neu gebaut) -- CONFIG_MBEDTLS_DES_C in
//    sdkconfig.defaults hat darauf keinen Einfluss, das Linken gegen
//    mbedtls_des3_* schlaegt deshalb immer mit "undefined reference" fehl,
//    egal was in sdkconfig steht. AES (mbedtls_aes_*) ist dagegen bereits
//    im vorkompilierten Binary enthalten und nutzt weiterhin mbedtls.

#include <Arduino.h>
#include <Adafruit_PN532.h>
#include <mbedtls/aes.h>
#include <esp_random.h>

extern Adafruit_PN532 nfc;

struct DesfireVersion {
  uint8_t hwVendorId, hwType, hwSubType, hwMajorVersion, hwMinorVersion, hwStorageSize, hwProtocol;
  uint8_t swVendorId, swType, swSubType, swMajorVersion, swMinorVersion, swStorageSize, swProtocol;
  uint8_t uid[7];
  uint8_t batchNo[5];
  uint8_t productionWeek, productionYear;
};

struct DesfireFileSettings {
  uint8_t fileType;   // 0x00 Standard, 0x01 Backup, 0x02 Value, 0x03 Linear Record, 0x04 Cyclic Record
  uint8_t commMode;   // 0x00 Plain, 0x01 MACed, 0x03 Enciphered
  uint32_t fileSize;  // nur fuer Standard/Backup Data Files befuellt
};

// Menschenlesbarer Name fuer die haeufigsten DESFire-Statuscodes -- fuer
// Serial-/SD-Log-Diagnose.
inline const char *desfireStatusName(uint8_t status) {
  switch (status) {
    case 0x00: return "OPERATION_OK";
    case 0x0C: return "NO_CHANGES";
    case 0x1C: return "ILLEGAL_COMMAND_CODE";
    case 0x1E: return "INTEGRITY_ERROR";
    case 0x40: return "NO_SUCH_KEY";
    case 0x7E: return "LENGTH_ERROR";
    case 0x9D: return "PERMISSION_DENIED";
    case 0x9E: return "PARAMETER_ERROR";
    case 0xA0: return "APPLICATION_NOT_FOUND";
    case 0xAE: return "AUTHENTICATION_ERROR";
    case 0xAF: return "ADDITIONAL_FRAME";
    case 0xBE: return "BOUNDARY_ERROR";
    case 0xCA: return "COMMAND_ABORTED";
    case 0xDE: return "DUPLICATE_ERROR";
    case 0xEE: return "MEMORY_ERROR";
    case 0xF0: return "FILE_NOT_FOUND";
    case 0xF1: return "FILE_INTEGRITY_ERROR";
    case 0xFF: return "PN532_TRANSCEIVE_FEHLGESCHLAGEN";
    default:   return "UNBEKANNT";
  }
}

// Sendet ein natives DESFire-Kommando und sammelt automatisch weitere
// Antwort-Frames ein, solange die Karte Status 0xAF (ADDITIONAL_FRAME)
// zurueckgibt (abgeholt mit dem GetAdditionalFrame-Kommando 0xAF ohne
// Parameter). Gibt den LETZTEN Statusbyte zurueck; alle Nutzdaten (ohne
// Statusbytes) landen in outBuf/outLen. NICHT fuer Authenticate-Kommandos
// verwenden -- dort hat 0xAF eine andere Bedeutung (Fortsetzung der
// Challenge-Response, nicht "mehr Daten derselben Antwort") und der
// naechste Frame enthaelt echte Nutzdaten, kein leeres GetAdditionalFrame.
inline uint8_t desfireTransceive(uint8_t cmd, const uint8_t *params, uint8_t paramsLen,
                                  uint8_t *outBuf, uint16_t *outLen, uint16_t outBufCap) {
  *outLen = 0;
  // 40 statt vormals 16 Byte -- ChangeKey (0xC4) braucht bis zu 1 (Cmd) +
  // 1 (KeyNo) + 32 (AES-Kryptogramm, aufgefuellt auf 2 AES-Bloecke) = 34
  // Byte. PN532_PACKBUFFSIZ ist per Pflicht-Patch (siehe README.md) auf
  // 255 erhoeht, das ist also nicht der begrenzende Faktor.
  uint8_t sendBuf[40];
  sendBuf[0] = cmd;
  if (paramsLen > 0) memcpy(sendBuf + 1, params, paramsLen);

  uint8_t respBuf[64];
  uint8_t respLen = sizeof(respBuf);
  if (!nfc.inDataExchange(sendBuf, paramsLen + 1, respBuf, &respLen) || respLen < 1) {
    return 0xFF;
  }
  uint8_t status = respBuf[0];
  uint16_t dataLen = respLen - 1;
  if (*outLen + dataLen > outBufCap) dataLen = outBufCap - *outLen;
  memcpy(outBuf + *outLen, respBuf + 1, dataLen);
  *outLen += dataLen;

  while (status == 0xAF) {
    uint8_t afCmd = 0xAF;
    respLen = sizeof(respBuf);
    if (!nfc.inDataExchange(&afCmd, 1, respBuf, &respLen) || respLen < 1) return 0xFF;
    status = respBuf[0];
    dataLen = respLen - 1;
    if (*outLen + dataLen > outBufCap) dataLen = outBufCap - *outLen;
    memcpy(outBuf + *outLen, respBuf + 1, dataLen);
    *outLen += dataLen;
  }
  return status;
}

inline bool desfireGetVersion(DesfireVersion &v) {
  uint8_t buf[32];
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0x60, nullptr, 0, buf, &len, sizeof(buf));
  if (status != 0x00 || len < 28) {
    logMsg("desfireGetVersion(): %s (status=0x%02X, len=%u)\n", desfireStatusName(status), status, len);
    return false;
  }
  v.hwVendorId = buf[0]; v.hwType = buf[1]; v.hwSubType = buf[2];
  v.hwMajorVersion = buf[3]; v.hwMinorVersion = buf[4]; v.hwStorageSize = buf[5]; v.hwProtocol = buf[6];
  v.swVendorId = buf[7]; v.swType = buf[8]; v.swSubType = buf[9];
  v.swMajorVersion = buf[10]; v.swMinorVersion = buf[11]; v.swStorageSize = buf[12]; v.swProtocol = buf[13];
  memcpy(v.uid, buf + 14, 7);
  memcpy(v.batchNo, buf + 21, 5);
  v.productionWeek = buf[26];
  v.productionYear = buf[27];
  return true;
}

inline uint8_t desfireGetApplicationIDs(uint8_t aids[][3], uint8_t maxApps) {
  uint8_t buf[96];
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0x6A, nullptr, 0, buf, &len, sizeof(buf));
  if (status != 0x00) {
    logMsg("desfireGetApplicationIDs(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return 0;
  }
  uint8_t count = len / 3;
  if (count > maxApps) count = maxApps;
  for (uint8_t i = 0; i < count; i++) memcpy(aids[i], buf + i * 3, 3);
  return count;
}

inline bool desfireSelectApplication(const uint8_t aid[3]) {
  uint8_t buf[8];
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0x5A, aid, 3, buf, &len, sizeof(buf));
  if (status != 0x00) {
    logMsg("desfireSelectApplication(): %s (status=0x%02X)\n", desfireStatusName(status), status);
  }
  return status == 0x00;
}

inline uint8_t desfireGetFileIDs(uint8_t fileIds[], uint8_t maxFiles) {
  uint8_t buf[32];
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0x6F, nullptr, 0, buf, &len, sizeof(buf));
  if (status != 0x00) {
    logMsg("desfireGetFileIDs(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return 0;
  }
  // An echter Hardware wurden hier schon deutlich mehr "Dateien" gemeldet
  // als je angelegt wurden (bis zu 9 statt der erwarteten 1, mit klar
  // unplausiblen Dateinummern wie 218) -- der Adafruit-Treiber uebernimmt
  // die Laenge nur, wenn die vom PN532-CHIP SELBST mitgesendete
  // Laengen-Pruefsumme (LCS) passt (siehe Adafruit_PN532.cpp,
  // inDataExchange()), das Problem liegt also vermutlich im PN532-
  // internen Zielantwortspeicher (Restdaten des vorangegangenen 8-Byte-
  // Authentifizierungsaustauschs), nicht im Treiber oder unserem Code
  // (siehe firmware/README.md, Abschnitt "Stage 6"). Ein echter
  // Speichermuell-Fehlschlag wiederholt sich vermutlich nicht IDENTISCH
  // -- deshalb: bei verdaechtig hoher Anzahl (>1) die Abfrage bis zu 2x
  // wiederholen und nur ein Ergebnis uebernehmen, das sich in zwei
  // aufeinanderfolgenden Abfragen exakt bestaetigt.
  for (int attempt = 1; len > 1 && attempt <= 2; attempt++) {
    logMsg("desfireGetFileIDs(): len=%u (Versuch %d, mehr als die erwartete 1 Datei) -- pruefe erneut", len, attempt);
    logHex("rohe Datei-IDs", buf, len < 32 ? len : 32);

    uint8_t buf2[32];
    uint16_t len2 = 0;
    uint8_t status2 = desfireTransceive(0x6F, nullptr, 0, buf2, &len2, sizeof(buf2));
    if (status2 != 0x00) {
      logMsg("desfireGetFileIDs(): Wiederholung fehlgeschlagen (status=0x%02X)", status2);
      break;
    }
    if (len2 == len && memcmp(buf, buf2, len) == 0) {
      logMsg("desfireGetFileIDs(): Wiederholung bestaetigt dasselbe Ergebnis -- wird uebernommen.");
      break;
    }
    logMsg("desfireGetFileIDs(): Wiederholung ergab ein ANDERES Ergebnis (len=%u) -- "
           "vermutlich war die vorherige Antwort PN532-Speichermuell.", len2);
    memcpy(buf, buf2, len2 < sizeof(buf2) ? len2 : sizeof(buf2));
    len = len2;
  }
  uint8_t count = len;
  if (count > maxFiles) count = maxFiles;
  memcpy(fileIds, buf, count);
  return count;
}

inline bool desfireGetFileSettings(uint8_t fileId, DesfireFileSettings &out) {
  uint8_t buf[24];
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0xF5, &fileId, 1, buf, &len, sizeof(buf));
  if (status != 0x00 || len < 1) {
    logMsg("desfireGetFileSettings(file %u): %s (status=0x%02X)\n", fileId, desfireStatusName(status), status);
    return false;
  }
  out.fileType = buf[0];
  out.commMode = buf[1] & 0x03;
  // Layout Standard/Backup Data File: FileType(1) CommSettings(1) AccessRights(2) FileSize(3, LSB zuerst)
  out.fileSize = (out.fileType == 0x00 || out.fileType == 0x01) && len >= 7
                     ? ((uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16))
                     : 0;
  return true;
}

// Liest eine Standard-/Backup-Data-Datei komplett (Offset 0, Laenge 0 =
// "alles"). Gibt die Anzahl gelesener Bytes zurueck (0 bei Fehler).
inline uint16_t desfireReadData(uint8_t fileId, uint8_t *outBuf, uint16_t outBufCap) {
  uint8_t params[7] = { fileId, 0, 0, 0, 0, 0, 0 }; // Offset=0, Laenge=0 (=alles)
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0xBD, params, 7, outBuf, &len, outBufCap);
  if (status != 0x00) {
    logMsg("desfireReadData(file %u): %s (status=0x%02X)\n", fileId, desfireStatusName(status), status);
    return 0;
  }
  return len;
}

// Liest ALLE Datensaetze einer Record-Datei (Linear/Cyclic). recordSize
// muss aus GetFileSettings() bekannt sein (bei Record-Dateien andere
// Antwortstruktur als bei Data-Dateien -- hier vereinfacht: liest so viele
// Bytes wie outBufCap zulaesst, Aufrufer schneidet nach recordSize auf).
inline uint16_t desfireReadRecords(uint8_t fileId, uint8_t *outBuf, uint16_t outBufCap) {
  uint8_t params[7] = { fileId, 0, 0, 0, 0, 0, 0 }; // Offset=0, AnzahlRecords=0 (=alle)
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0xBB, params, 7, outBuf, &len, outBufCap);
  if (status != 0x00) {
    logMsg("desfireReadRecords(file %u): %s (status=0x%02X)\n", fileId, desfireStatusName(status), status);
    return 0;
  }
  return len;
}

// Rotiert einen Byte-Block um 1 Byte nach links (fuer die
// ISO/IEC-9798-2-Challenge-Response-Rotation von RndA/RndB).
inline void desfireRotateLeft1(uint8_t *buf, size_t len) {
  uint8_t first = buf[0];
  memmove(buf, buf + 1, len - 1);
  buf[len - 1] = first;
}

// Schreibt einen int32_t LSB-zuerst (DESFire-Konvention fuer alle
// Mehrbyte-Felder) -- explizit statt memcpy(), um nicht von der
// Host-Byte-Ordnung abzuhaengen (auf ESP32-S3/Xtensa zwar ohnehin
// Little-Endian, aber so bleibt es unabhaengig davon korrekt).
inline void desfirePutLE32(uint8_t *dst, int32_t v) {
  uint32_t u = (uint32_t)v;
  dst[0] = (uint8_t)(u & 0xFF);
  dst[1] = (uint8_t)((u >> 8) & 0xFF);
  dst[2] = (uint8_t)((u >> 16) & 0xFF);
  dst[3] = (uint8_t)((u >> 24) & 0xFF);
}
inline int32_t desfireGetLE32(const uint8_t *src) {
  uint32_t u = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
  return (int32_t)u;
}

// CRC-A (ISO/IEC 14443-3) -- fuer ChangeKey-Kryptogramme mit Legacy-DES/
// 2K3DES-Schluesseln. Poly 0x8408 (reflektierte Form von 0x1021), Init
// 0x6363, kein finales XOR, Ergebnis wird LSB-zuerst angehaengt. Derselbe
// Algorithmus, den PN532/PICCs ueberall in ISO14443A verwenden.
inline uint16_t desfireCrc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0x6363;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0x8408;
      else crc = crc >> 1;
    }
  }
  return crc;
}

// CRC-32 (DESFire-Variante) -- fuer ChangeKey-Kryptogramme mit AES-/
// 3K3DES-Schluesseln. Poly 0xEDB88320 (reflektierte Form, wie beim
// bekannten zlib/PKZIP-CRC32), Init 0xFFFFFFFF, ABER OHNE das bei
// zlib uebliche finale XOR mit 0xFFFFFFFF -- DESFire laesst das weg.
// Ergebnis wird LSB-zuerst angehaengt.
inline uint32_t desfireCrc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
      else crc = crc >> 1;
    }
  }
  return crc;
}

// ---------------------------------------------------------------------
// Eigene DES/2K3DES-Implementierung (FIPS 46-3), OHNE mbedtls.
//
// Grund: ESP-IDFs mbedtls-Komponente wird bei framework=arduino als FERTIG
// VORKOMPILIERTES Binary mit dem Framework-Paket ausgeliefert (nicht aus
// Quellcode neu gebaut) -- CONFIG_MBEDTLS_DES_C=y in sdkconfig.defaults
// hat darauf KEINEN Einfluss, das Linken gegen mbedtls_des3_* schlaegt
// deshalb mit "undefined reference" fehl, egal was in sdkconfig steht
// (mbedtls_aes_* ist dagegen bereits im vorkompilierten Binary enthalten,
// daher funktioniert die AES-Authentifizierung unten weiterhin ueber
// mbedtls). Um nicht von der (nicht beeinflussbaren) Cipher-Auswahl des
// vorkompilierten Frameworks abhaengig zu sein, ist DES hier komplett
// selbst implementiert -- Standard-FIPS-46-3-Tabellen, unveraendert seit
// 1977, in jedem DES-Lehrbuch/jeder Referenzimplementierung identisch.
// ---------------------------------------------------------------------

static const uint8_t DES_IP[64] = {
  58,50,42,34,26,18,10,2, 60,52,44,36,28,20,12,4,
  62,54,46,38,30,22,14,6, 64,56,48,40,32,24,16,8,
  57,49,41,33,25,17,9,1,  59,51,43,35,27,19,11,3,
  61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7
};
static const uint8_t DES_FP[64] = {
  40,8,48,16,56,24,64,32, 39,7,47,15,55,23,63,31,
  38,6,46,14,54,22,62,30, 37,5,45,13,53,21,61,29,
  36,4,44,12,52,20,60,28, 35,3,43,11,51,19,59,27,
  34,2,42,10,50,18,58,26, 33,1,41,9,49,17,57,25
};
static const uint8_t DES_E[48] = {
  32,1,2,3,4,5, 4,5,6,7,8,9, 8,9,10,11,12,13, 12,13,14,15,16,17,
  16,17,18,19,20,21, 20,21,22,23,24,25, 24,25,26,27,28,29, 28,29,30,31,32,1
};
static const uint8_t DES_P[32] = {
  16,7,20,21, 29,12,28,17, 1,15,23,26, 5,18,31,10,
  2,8,24,14, 32,27,3,9, 19,13,30,6, 22,11,4,25
};
static const uint8_t DES_PC1[56] = {
  57,49,41,33,25,17,9, 1,58,50,42,34,26,18,
  10,2,59,51,43,35,27, 19,11,3,60,52,44,36,
  63,55,47,39,31,23,15, 7,62,54,46,38,30,22,
  14,6,61,53,45,37,29, 21,13,5,28,20,12,4
};
static const uint8_t DES_PC2[48] = {
  14,17,11,24,1,5, 3,28,15,6,21,10, 23,19,12,4,26,8, 16,7,27,20,13,2,
  41,52,31,37,47,55, 30,40,51,45,33,48, 44,49,39,56,34,53, 46,42,50,36,29,32
};
static const uint8_t DES_SHIFTS[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t DES_SBOX[8][64] = {
  {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7, 0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
   4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0, 15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
  {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10, 3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
   0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15, 13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
  {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8, 13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
   13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7, 1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
  {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15, 13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
   10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4, 3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
  {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9, 14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
   4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14, 11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
  {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11, 10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
   9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6, 4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
  {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1, 13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
   1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2, 6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
  {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7, 1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
   7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8, 2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
};

// Liest outBits Bits (1-indexiert, MSB-zuerst) aus den unteren inBits Bits
// von "in" gemaess table und packt sie MSB-zuerst in das Ergebnis.
static uint64_t desPermute(uint64_t in, const uint8_t *table, int outBits, int inBits) {
  uint64_t out = 0;
  for (int i = 0; i < outBits; i++) {
    int bitPos = table[i];
    uint64_t bit = (in >> (inBits - bitPos)) & 1ULL;
    out = (out << 1) | bit;
  }
  return out;
}

static uint64_t desBytesToBlock(const uint8_t b[8]) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
  return v;
}
static void desBlockToBytes(uint64_t v, uint8_t b[8]) {
  for (int i = 7; i >= 0; i--) { b[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

// Schluesselplan: 16 Rundenschluessel (je 48 Bit) aus einem 8-Byte-DES-Schluessel.
static void desKeySchedule(const uint8_t key8[8], uint64_t subkeys[16]) {
  uint64_t key64 = desBytesToBlock(key8);
  uint64_t pc1 = desPermute(key64, DES_PC1, 56, 64);
  uint32_t c = (uint32_t)(pc1 >> 28) & 0x0FFFFFFFu;
  uint32_t d = (uint32_t)pc1 & 0x0FFFFFFFu;
  for (int round = 0; round < 16; round++) {
    int shift = DES_SHIFTS[round];
    c = ((c << shift) | (c >> (28 - shift))) & 0x0FFFFFFFu;
    d = ((d << shift) | (d >> (28 - shift))) & 0x0FFFFFFFu;
    uint64_t cd = ((uint64_t)c << 28) | d;
    subkeys[round] = desPermute(cd, DES_PC2, 48, 56);
  }
}

static uint32_t desF(uint32_t r, uint64_t subkey) {
  uint64_t expanded = desPermute((uint64_t)r, DES_E, 48, 32);
  uint64_t x = expanded ^ subkey;
  uint32_t sOut = 0;
  for (int i = 0; i < 8; i++) {
    int shift = 42 - i * 6;
    uint8_t six = (uint8_t)((x >> shift) & 0x3F);
    uint8_t row = (uint8_t)(((six & 0x20) >> 4) | (six & 0x01));
    uint8_t col = (uint8_t)((six >> 1) & 0x0F);
    uint8_t sVal = DES_SBOX[i][row * 16 + col];
    sOut = (sOut << 4) | sVal;
  }
  return (uint32_t)desPermute((uint64_t)sOut, DES_P, 32, 32);
}

// Ein 8-Byte-Block, Single-DES (16-Runden-Feistel-Netzwerk).
static void desCryptBlock(const uint64_t subkeys[16], bool encrypt, const uint8_t in[8], uint8_t out[8]) {
  uint64_t block = desBytesToBlock(in);
  uint64_t ip = desPermute(block, DES_IP, 64, 64);
  uint32_t l = (uint32_t)(ip >> 32);
  uint32_t r = (uint32_t)ip;
  for (int round = 0; round < 16; round++) {
    int idx = encrypt ? round : (15 - round);
    uint32_t newR = l ^ desF(r, subkeys[idx]);
    l = r;
    r = newR;
  }
  // Kein abschliessendes Vertauschen -- Vorausgabe ist R16||L16.
  uint64_t preOutput = ((uint64_t)r << 32) | l;
  uint64_t fp = desPermute(preOutput, DES_FP, 64, 64);
  desBlockToBytes(fp, out);
}

struct Des3Keys {
  uint64_t k1[16];
  uint64_t k2[16];
};

// K1 = key16[0..7], K2 = key16[8..15] (bei einem 8-Byte-Werksschluessel,
// hier auf 16 Byte dupliziert -- siehe Aufrufer -- ist K1=K2, was 2K3DES
// auf Single-DES reduziert, exakt das erwartete Verhalten).
static void desfireDes3SetKey(const uint8_t key16[16], Des3Keys &out) {
  desKeySchedule(key16, out.k1);
  desKeySchedule(key16 + 8, out.k2);
}

// 2K3DES EDE (Encrypt-Decrypt-Encrypt mit K1,K2,K1) fuer einen Block.
static void des3CryptBlockEde(const Des3Keys &keys, bool encrypt, const uint8_t in[8], uint8_t out[8]) {
  uint8_t tmp1[8], tmp2[8];
  if (encrypt) {
    desCryptBlock(keys.k1, true, in, tmp1);
    desCryptBlock(keys.k2, false, tmp1, tmp2);
    desCryptBlock(keys.k1, true, tmp2, out);
  } else {
    desCryptBlock(keys.k1, false, in, tmp1);
    desCryptBlock(keys.k2, true, tmp1, tmp2);
    desCryptBlock(keys.k1, false, tmp2, out);
  }
}

// 2K3DES-CBC (mit K1=K2=key16[0..7], funktional identisch zu Single-DES
// fuer den haeufigen Fall eines 8-Byte-Werksschluessels, der hier auf 16
// Byte dupliziert wird -- siehe Aufrufer). IV wird NICHT ueber mehrere
// Aufrufe verkettet, das macht die Authentifizierungslogik unten selbst
// (jeder Aufruf bekommt seinen IV explizit uebergeben).
inline void desfireDes3Cbc(const uint8_t key16[16], const uint8_t iv[8], bool encrypt,
                            const uint8_t *in, uint8_t *out, size_t len) {
  Des3Keys keys;
  desfireDes3SetKey(key16, keys);
  uint8_t ivBuf[8];
  memcpy(ivBuf, iv, 8);
  size_t blocks = len / 8;
  if (encrypt) {
    for (size_t b = 0; b < blocks; b++) {
      uint8_t xored[8];
      for (int i = 0; i < 8; i++) xored[i] = in[b * 8 + i] ^ ivBuf[i];
      des3CryptBlockEde(keys, true, xored, out + b * 8);
      memcpy(ivBuf, out + b * 8, 8);
    }
  } else {
    for (size_t b = 0; b < blocks; b++) {
      uint8_t decrypted[8];
      des3CryptBlockEde(keys, false, in + b * 8, decrypted);
      for (int i = 0; i < 8; i++) out[b * 8 + i] = decrypted[i] ^ ivBuf[i];
      memcpy(ivBuf, in + b * 8, 8);
    }
  }
}

inline void desfireAesCbc(const uint8_t key16[16], const uint8_t iv[16], bool encrypt,
                           const uint8_t *in, uint8_t *out, size_t len) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (encrypt) mbedtls_aes_setkey_enc(&ctx, key16, 128);
  else mbedtls_aes_setkey_dec(&ctx, key16, 128);
  uint8_t ivBuf[16];
  memcpy(ivBuf, iv, 16);
  mbedtls_aes_crypt_cbc(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, len, ivBuf, in, out);
  mbedtls_aes_free(&ctx);
}

// Formatiert bis zu 16 Byte als Hex-String fuers Log -- reine Diagnose-
// Hilfsfunktion, um bei einem Authentifizierungsfehlschlag die tatsaechlich
// ausgetauschten Rohdaten sichtbar zu machen (z. B. um zu pruefen, ob eine
// fehlgeschlagene RndA-Rueckpruefung an voellig zufaelligen oder eher
// "fast richtigen" Bytes liegt).
inline void logHex(const char *label, const uint8_t *data, size_t len) {
  char hex[56] = "";
  size_t pos = 0;
  for (size_t i = 0; i < len && pos + 3 < sizeof(hex); i++) {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
  }
  logMsg("  %s: %s", label, hex);
}

// 2K3DES-Authentifizierung, ISO/IEC-9798-2-3-Pass-Mutual-Authentication.
// key16 = 16-Byte-Schluessel (Werks-Default: 16 Nullbytes). sessionKeyOut
// wird bei Erfolg befuellt. ivOut (falls nicht nullptr) bekommt den IV
// fuer ein direkt folgendes verschluesseltes Kommando in DERSELBEN
// Sitzung (z. B. ChangeKey, siehe desfireChangeKeySame()) -- das ist NACH
// erfolgreicher Authentifizierung wieder 8 Nullbytes, NICHT der letzte
// ausgetauschte Chiffretext-Block (gegen echte Hardware per Proxmark3
// verifiziert, siehe unten).
//
// Verwendet Kommando 0x1A (AUTHENTICATE_ISO), NICHT 0x0A
// (AUTHENTICATE/"Legacy"): Mit 0x0A akzeptierte eine echte DESFire-EV3-
// Karte (Werks-Default-Schluessel) zwar formal Schritt 2 (Status 0x00),
// aber die eigene Rueckpruefung von Schritt 4 schlug IMMER fehl -- per
// unabhaengigem Proxmark3-Test (Kommando "hf mfdes auth -n 0 -t 2tdea -k
// <16 Nullbytes>", Firmware iceman-Fork) an genau dieser Karte bestaetigt:
// der Schluessel ist tatsaechlich der Werks-Default, Proxmark3
// authentifiziert erfolgreich -- ABER Proxmark3 verwendet dafuer intern
// "Secure channel: ev1" und damit Kommando 0x1A (nicht 0x0A) fuer einen
// Nicht-AES-Schluessel (siehe RfidResearchGroup/proxmark3,
// desfirecore.c::DesfireAuthenticateEV1(), Zeile ~1362: bei
// secureChannel==DACEV1 wird subcommand=MFDES_AUTHENTICATE_ISO=0x1A statt
// MFDES_AUTHENTICATE=0x0A gesendet). Kryptografie/IV-Verkettung sind
// zwischen 0x0A und 0x1A identisch (siehe Proxmark3-Quellcode) -- nur das
// Kommandobyte unterscheidet sich, aber die Karte behandelt beide
// offenbar unterschiedlich intern. Alte, an dieser Stelle hier ausprobierte
// Theorie ("Legacy-0x0A braucht Entschluesselungs- statt
// Verschluesselungs-Chiffre fuers PCD->PICC-Kryptogramm", nach
// libfreefare) war beim Werks-Nullschluessel mathematisch unwirksam
// (Ver-/Entschluesselung sind dort identisch) und hat das eigentliche
// Problem nicht behoben -- daher wieder normale Verschluesselung
// (encrypt=true unten), passend zu Proxmark3s EV1-Pfad.
inline bool desfireAuthDes3(uint8_t keyNo, const uint8_t key16[16], uint8_t sessionKeyOut[16],
                             uint8_t ivOut[8] = nullptr) {
  // Bis zu 3 komplette Versuche, ABER NUR falls die Karte in Schritt 2
  // bereits mit Status 0x00 (= Schluessel akzeptiert!) geantwortet hat und
  // erst UNSERE eigene RndA-Rueckpruefung fehlschlaegt. Ein wirklich
  // falscher Schluessel fuehrt (Blockchiffre-Entschluesselung mit falschem
  // Schluessel liefert Pseudozufall) mit ueberwaeltigender Wahrscheinlichkeit
  // schon in Schritt 2 zu Status 0xAE von der Karte selbst -- dort wird
  // sofort abgebrochen (kein sinnloses Retry). Erreichen wir dagegen die
  // RndA-Rueckpruefung mit Status 0x00, aber mit falschem Ergebnis, ist der
  // Schluessel mit an Sicherheit grenzender Wahrscheinlichkeit korrekt und
  // es handelt sich um einen einzelnen Uebertragungsfehler auf der
  // Luftschnittstelle -- ein erneuter (komplett neuer) Challenge-Response-
  // Durchlauf behebt das typischerweise.
  for (int attempt = 1; attempt <= 3; attempt++) {
    uint8_t send1[2] = { 0x1A, keyNo }; // AUTHENTICATE_ISO -- siehe Funktionskommentar (NICHT 0x0A)
    uint8_t resp[32];
    uint8_t respLen = sizeof(resp);
    if (!nfc.inDataExchange(send1, 2, resp, &respLen) || respLen < 9 || resp[0] != 0xAF) {
      logMsg("desfireAuthDes3: Schritt 1 unerwartet (status=0x%02X, len=%u)\n",
                    respLen > 0 ? resp[0] : 0xFF, respLen);
      return false;
    }
    uint8_t encRndB[8];
    memcpy(encRndB, resp + 1, 8);

    uint8_t ivZero[8] = {0};
    uint8_t rndB[8];
    desfireDes3Cbc(key16, ivZero, false, encRndB, rndB, 8);

    uint8_t rndA[8];
    esp_fill_random(rndA, 8);

    uint8_t rndBRot[8];
    memcpy(rndBRot, rndB, 8);
    desfireRotateLeft1(rndBRot, 8);

    uint8_t plainAB[16];
    memcpy(plainAB, rndA, 8);
    memcpy(plainAB + 8, rndBRot, 8);

    uint8_t encAB[16];
    desfireDes3Cbc(key16, encRndB, true, plainAB, encAB, 16); // IV = encRndB (Chiffretext-Chaining)

    uint8_t send2[17];
    send2[0] = 0xAF;
    memcpy(send2 + 1, encAB, 16);
    respLen = sizeof(resp);
    if (!nfc.inDataExchange(send2, 17, resp, &respLen) || respLen < 9 || resp[0] != 0x00) {
      logMsg("desfireAuthDes3: Schritt 2 unerwartet (status=0x%02X, len=%u) -- "
                    "vermutlich falscher Schluessel oder Karte hat Werksschluessel nicht mehr.\n",
                    respLen > 0 ? resp[0] : 0xFF, respLen);
      return false;
    }
    // Diagnose: exakte Antwortlaenge + KOMPLETTER Rohpuffer (nicht nur die
    // interpretierten 8 Byte) -- die "send"-Seite ist inzwischen bit-genau
    // gegen den tatsaechlichen Proxmark3-Algorithmus verifiziert (siehe
    // firmware/README.md Nachtrag 5), der Verdacht faellt deshalb auf die
    // EMPFANGSSEITE dieser konkreten Antwort: falls respLen != 9 waere,
    // wuerden wir hier faelschlich die ERSTEN 8 Byte einer laengeren/
    // anders aufgebauten Antwort als encRndAResp interpretieren.
    logMsg("desfireAuthDes3: Schritt-2-Antwort respLen=%u", respLen);
    logHex("resp[] roh (bis respLen)", resp, respLen);

    uint8_t encRndAResp[8];
    memcpy(encRndAResp, resp + 1, 8);

    uint8_t rndARot[8];
    desfireDes3Cbc(key16, encAB + 8, false, encRndAResp, rndARot, 8); // IV = letzter von uns gesendeter Block

    uint8_t rndAExpectedRot[8];
    memcpy(rndAExpectedRot, rndA, 8);
    desfireRotateLeft1(rndAExpectedRot, 8);

    if (memcmp(rndARot, rndAExpectedRot, 8) == 0) {
      // Sitzungsschluessel (2K3DES): RndA[0:4] + RndB[0:4] + RndA[4:8] + RndB[4:8].
      memcpy(sessionKeyOut, rndA, 4);
      memcpy(sessionKeyOut + 4, rndB, 4);
      memcpy(sessionKeyOut + 8, rndA + 4, 4);
      memcpy(sessionKeyOut + 12, rndB + 4, 4);
      // SONDERFALL K1==K2 (z. B. Werks-Default, 16 Nullbytes -- 2K3DES
      // reduziert dann auf Single-DES, siehe Kopfkommentar bei
      // desfireDes3SetKey()): dann muss die zweite Haelfte des
      // Sitzungsschluessels die ERSTE wiederholen, NICHT RndA[4:8]/
      // RndB[4:8] verwenden -- gegen Proxmark3 (desfirecore.c, Kommentar
      // "If the 3Des key first 8 bytes = 2nd 8 Bytes then we are really
      // using Singe Des... we need to set the session key such that the
      // 2nd 8 bytes = 1st 8 bytes") verifiziert. Betrifft direkt den
      // Werks-Default-Fall von "MASTER-PW SETZEN" (siehe
      // desfireChangeKeySame(), direkt nach dieser Authentifizierung).
      if (memcmp(key16, key16 + 8, 8) == 0) {
        memcpy(sessionKeyOut + 8, sessionKeyOut, 8);
      }
      // IV fuer ein direkt folgendes Kommando (z. B. ChangeKey) ist NACH
      // erfolgreicher Authentifizierung 8 Nullbytes, nicht der letzte
      // Chiffretext-Block -- gegen Proxmark3 (desfirecore.c:
      // "memset(dctx->IV, 0, ...)" direkt nach Session-Key-Berechnung)
      // verifiziert.
      if (ivOut) memset(ivOut, 0, 8);
      return true;
    }

    // Roh-Hex-Dump fuer den Fehlerfall -- reine Diagnose (siehe logHex()-
    // Kommentar), damit sich bei einem erneuten Fehlschlag an echter
    // Hardware nachrechnen laesst, ob die Abweichung "voellig zufaellig"
    // aussieht (spraeche fuer Uebertragungsfehler) oder ein erkennbares
    // Muster hat (spraeche fuer einen Logikfehler).
    logMsg("desfireAuthDes3: Diagnose Versuch %d:", attempt);
    logHex("EncRndB (von Karte, Schritt 1)", encRndB, 8);
    logHex("RndB (entschluesselt)", rndB, 8);
    logHex("RndA (generiert)", rndA, 8);
    logHex("EncAB[0:8] (gesendet)", encAB, 8);
    logHex("EncAB[8:16] (=IV Schritt 4)", encAB + 8, 8);
    logHex("EncRndAResp (von Karte)", encRndAResp, 8);
    logHex("RndARot (berechnet)", rndARot, 8);
    logHex("RndARot (erwartet)", rndAExpectedRot, 8);

    if (attempt < 3) {
      logMsg("desfireAuthDes3: RndA-Rueckpruefung fehlgeschlagen, obwohl die Karte den Schluessel in "
                    "Schritt 2 akzeptiert hat (Status 0x00) -- vermutlich Uebertragungsfehler, "
                    "neuer Versuch %d/3.\n", attempt + 1);
    } else {
      logMsg("desfireAuthDes3: RndA-Rueckpruefung nach 3 Versuchen weiterhin fehlgeschlagen "
                    "(Schluessel war laut Karten-Status 0x00 korrekt -- kein Schluesselproblem).");
    }
  }
  return false;
}

// AES-128-Authentifizierung (Kommando 0xAA), gleiche Grundstruktur wie
// desfireAuthDes3(), nur mit 16-Byte-Bloecken statt 8-Byte-Bloecken.
// ivOut siehe Kommentar bei desfireAuthDes3() (hier 16 Byte statt 8).
inline bool desfireAuthAes(uint8_t keyNo, const uint8_t key16[16], uint8_t sessionKeyOut[16],
                            uint8_t ivOut[16] = nullptr) {
  // Retry-Logik siehe ausfuehrlicher Kommentar bei desfireAuthDes3() -- nur
  // bei Status 0x00 in Schritt 2 UND fehlschlagender eigener RndA-
  // Rueckpruefung (= Schluessel von der Karte bereits akzeptiert, vermutlich
  // Uebertragungsfehler), NICHT bei Status-Fehlern (= echte Ablehnung).
  for (int attempt = 1; attempt <= 3; attempt++) {
    uint8_t send1[2] = { 0xAA, keyNo };
    uint8_t resp[40];
    uint8_t respLen = sizeof(resp);
    if (!nfc.inDataExchange(send1, 2, resp, &respLen) || respLen < 17 || resp[0] != 0xAF) {
      logMsg("desfireAuthAes: Schritt 1 unerwartet (status=0x%02X, len=%u)\n",
                    respLen > 0 ? resp[0] : 0xFF, respLen);
      return false;
    }
    uint8_t encRndB[16];
    memcpy(encRndB, resp + 1, 16);

    uint8_t ivZero[16] = {0};
    uint8_t rndB[16];
    desfireAesCbc(key16, ivZero, false, encRndB, rndB, 16);

    uint8_t rndA[16];
    esp_fill_random(rndA, 16);

    uint8_t rndBRot[16];
    memcpy(rndBRot, rndB, 16);
    desfireRotateLeft1(rndBRot, 16);

    uint8_t plainAB[32];
    memcpy(plainAB, rndA, 16);
    memcpy(plainAB + 16, rndBRot, 16);

    uint8_t encAB[32];
    desfireAesCbc(key16, encRndB, true, plainAB, encAB, 32);

    uint8_t send2[33];
    send2[0] = 0xAF;
    memcpy(send2 + 1, encAB, 32);
    respLen = sizeof(resp);
    if (!nfc.inDataExchange(send2, 33, resp, &respLen) || respLen < 17 || resp[0] != 0x00) {
      logMsg("desfireAuthAes: Schritt 2 unerwartet (status=0x%02X, len=%u) -- "
                    "vermutlich falscher Schluessel oder Karte hat Werksschluessel nicht mehr.\n",
                    respLen > 0 ? resp[0] : 0xFF, respLen);
      return false;
    }
    uint8_t encRndAResp[16];
    memcpy(encRndAResp, resp + 1, 16);

    uint8_t rndARot[16];
    desfireAesCbc(key16, encAB + 16, false, encRndAResp, rndARot, 16);

    uint8_t rndAExpectedRot[16];
    memcpy(rndAExpectedRot, rndA, 16);
    desfireRotateLeft1(rndAExpectedRot, 16);

    if (memcmp(rndARot, rndAExpectedRot, 16) == 0) {
      // Sitzungsschluessel (AES): RndA[0:4] + RndB[0:4] + RndA[12:16] + RndB[12:16]
      memcpy(sessionKeyOut, rndA, 4);
      memcpy(sessionKeyOut + 4, rndB, 4);
      memcpy(sessionKeyOut + 8, rndA + 12, 4);
      memcpy(sessionKeyOut + 12, rndB + 12, 4);
      // IV nach erfolgreicher Authentifizierung: 16 Nullbytes, siehe
      // Kommentar bei desfireAuthDes3()/ivOut.
      if (ivOut) memset(ivOut, 0, 16);
      return true;
    }

    // Roh-Hex-Dump siehe Kommentar bei desfireAuthDes3().
    logMsg("desfireAuthAes: Diagnose Versuch %d:", attempt);
    logHex("EncRndB (von Karte, Schritt 1)", encRndB, 16);
    logHex("RndB (entschluesselt)", rndB, 16);
    logHex("RndA (generiert)", rndA, 16);
    logHex("EncAB[0:16] (gesendet)", encAB, 16);
    logHex("EncAB[16:32] (=IV Schritt 4)", encAB + 16, 16);
    logHex("EncRndAResp (von Karte)", encRndAResp, 16);
    logHex("RndARot (berechnet)", rndARot, 16);
    logHex("RndARot (erwartet)", rndAExpectedRot, 16);

    if (attempt < 3) {
      logMsg("desfireAuthAes: RndA-Rueckpruefung fehlgeschlagen, obwohl die Karte den Schluessel in "
                    "Schritt 2 akzeptiert hat (Status 0x00) -- vermutlich Uebertragungsfehler, "
                    "neuer Versuch %d/3.\n", attempt + 1);
    } else {
      logMsg("desfireAuthAes: RndA-Rueckpruefung nach 3 Versuchen weiterhin fehlgeschlagen "
                    "(Schluessel war laut Karten-Status 0x00 korrekt -- kein Schluesselproblem).");
    }
  }
  return false;
}

// Probiert Legacy-2K3DES zuerst (haeufiger bei DESFire EV0), dann AES
// (haeufiger bei DESFire EV1+), jeweils mit EINEM gegebenen 16-Byte-
// Schluessel. ivOut (16 Byte Puffer, bei 2K3DES sind nur die ersten 8
// gueltig) bekommt den IV fuer ein direkt folgendes verschluesseltes
// Kommando (siehe desfireAuthDes3()/desfireAuthAes()). isAesOut zeigt an,
// welcher Cipher es war (fuer ChangeKey wichtig -- unterschiedliches
// Kryptogrammformat).
inline bool desfireAuthWithKey(uint8_t keyNo, const uint8_t key16[16], uint8_t sessionKeyOut[16],
                                uint8_t ivOut[16], const char **cipherNameOut, bool *isAesOut) {
  if (desfireAuthDes3(keyNo, key16, sessionKeyOut, ivOut)) {
    *cipherNameOut = "2K3DES";
    if (isAesOut) *isAesOut = false;
    return true;
  }
  if (desfireAuthAes(keyNo, key16, sessionKeyOut, ivOut)) {
    *cipherNameOut = "AES";
    if (isAesOut) *isAesOut = true;
    return true;
  }
  *cipherNameOut = "keiner";
  return false;
}

// Wie oben, aber immer mit dem Werks-Default-Schluessel (16 Nullbytes) --
// unveraendertes Verhalten/Signatur gegenueber der urspruenglichen
// Version dieser Funktion (Aufrufer in 05_pn532_spi.ino unveraendert).
inline bool desfireAuthDefaultKey(uint8_t keyNo, uint8_t sessionKeyOut[16], const char **cipherNameOut) {
  static const uint8_t zeroKey[16] = {0};
  uint8_t ivDummy[16];
  return desfireAuthWithKey(keyNo, zeroKey, sessionKeyOut, ivDummy, cipherNameOut, nullptr);
}

// Probiert erst den Werks-Default-Schluessel (16 Nullbytes), dann einen
// gegebenen Custom-Schluessel -- fuer Operationen, bei denen der aktuelle
// Kartenzustand (noch Werksschluessel oder schon per "Master PW setzen"
// umgestellt) nicht im Voraus bekannt ist. usedCustomOut zeigt an, welcher
// der beiden es letztlich war.
inline bool desfireAuthEitherKey(uint8_t keyNo, const uint8_t customKey16[16], uint8_t sessionKeyOut[16],
                                  uint8_t ivOut[16], const char **cipherNameOut, bool *isAesOut,
                                  bool *usedCustomOut) {
  static const uint8_t zeroKey[16] = {0};
  if (desfireAuthWithKey(keyNo, zeroKey, sessionKeyOut, ivOut, cipherNameOut, isAesOut)) {
    *usedCustomOut = false;
    return true;
  }
  if (desfireAuthWithKey(keyNo, customKey16, sessionKeyOut, ivOut, cipherNameOut, isAesOut)) {
    *usedCustomOut = true;
    return true;
  }
  *usedCustomOut = false;
  return false;
}

// ---------------------------------------------------------------------
// Schreibende DESFire-Kommandos: ChangeKey, CreateApplication,
// DeleteApplication, CreateValueFile, Credit, Debit, GetValue,
// CommitTransaction.
//
// SICHERHEITSHINWEIS ChangeKey: Das Kryptogramm enthaelt eine CRC, die
// die KARTE SELBST nach dem Entschluesseln prueft. Stimmen IV,
// Byte-Layout oder CRC hier nicht exakt mit der NXP-Spezifikation
// ueberein, verwirft die Karte das Kommando (Status != 0x00) -- der
// Schluessel bleibt dann UNVERAENDERT, es entsteht kein "kaputter"
// Schluessel-Zustand. Trotzdem: dieser Code wurde NICHT an echter
// DESFire-Hardware getestet (siehe Kopfkommentar der Datei).
// ---------------------------------------------------------------------

// ChangeKey (0xC4) NUR fuer den Fall "eigenen, gerade authentifizierten
// Schluessel aendern" (KeyNo == mit diesem Schluessel authentifiziert) --
// genau das macht "Master PW setzen"/"Auf Standard setzen": erst mit dem
// AKTUELLEN Schluessel authentifizieren (desfireAuthEitherKey()), dann
// DENSELBEN KeyNo per ChangeKey auf den NEUEN Wert setzen. Der allgemeine
// Fall "einen ANDEREN Schluessel als den authentifizierten aendern" (mit
// XOR-Verknuepfung von Alt-/Neuschluessel im Kryptogramm) ist NICHT
// implementiert, da hier nirgends gebraucht.
//
// Kryptogramm-Layout haengt vom Sicherheitskanal ab (D40 vs. EV1, siehe
// Proxmark3 desfirecore.c::DesfireChangeKey() -- direkt von GitHub
// geladen und gelesen, nicht nur zusammengefasst). Da desfireAuthDes3()
// inzwischen Kommando 0x1A (AUTHENTICATE_ISO) verwendet (siehe dortiger
// Kommentar), laeuft die Sitzung ueber den EV1-Kanal, und ChangeKey MUSS
// dessen Konvention nutzen, NICHT die D40-Konvention:
//   2K3DES (EV1): NewKey(16) + CRC32(0xC4,KeyNo,NewKey)(4) -> auf 24 Byte
//           nullgepolstert, 2K3DES-CBC mit dem Sitzungsschluessel,
//           IV = 8 Nullbytes (Startzustand direkt nach erfolgreicher
//           Authentifizierung, siehe ivOut-Kommentar bei
//           desfireAuthDes3()). FRUEHER (falsch, D40-Konvention) wurde
//           hier CRC16 NUR ueber NewKey verwendet (ohne Kommando-Header)
//           -- das schlug an echter EV3-Hardware mit INTEGRITY_ERROR
//           (Status 0x1E) fehl, weil die Karte inzwischen ueber den
//           EV1-Kanal authentifiziert ist und dort CRC32 UEBER
//           Kommandobyte+KeyNo+NewKey erwartet (Proxmark3-Kommentar:
//           "EV1 Checksum must cover: <KeyNo> <PrevKey XOR Newkey>").
//   AES (immer EV1-artig): NewKey(16) + KeyVersion(1) +
//           CRC32(0xC4,KeyNo,NewKey,KeyVersion)(4) -> auf 32 Byte
//           nullgepolstert, AES-CBC mit dem Sitzungsschluessel,
//           IV = 16 Nullbytes. War schon vorher korrekt (deckte den
//           Kommando-Header immer schon mit ab).
inline bool desfireChangeKeySame(uint8_t keyNo, const uint8_t newKey16[16],
                                  const uint8_t sessionKey16[16], const uint8_t iv[16], bool isAes) {
  uint8_t plain[32] = {0};
  uint8_t cipher[32];
  size_t totalLen;

  if (isAes) {
    uint8_t header[19];
    header[0] = 0xC4;
    header[1] = keyNo;
    memcpy(header + 2, newKey16, 16);
    header[18] = 0x00; // Key-Version -- 0 fuer neu angelegte/zurueckgesetzte Schluessel
    uint32_t crc = desfireCrc32(header, sizeof(header));
    memcpy(plain, newKey16, 16);
    plain[16] = 0x00; // Key-Version, siehe oben
    plain[17] = (uint8_t)(crc & 0xFF);
    plain[18] = (uint8_t)((crc >> 8) & 0xFF);
    plain[19] = (uint8_t)((crc >> 16) & 0xFF);
    plain[20] = (uint8_t)((crc >> 24) & 0xFF);
    totalLen = 32; // 21 Nutzbyte auf 32 (2 AES-Bloecke) nullgepolstert
    desfireAesCbc(sessionKey16, iv, true, plain, cipher, totalLen);
  } else {
    // CRC32 ueber Kommandobyte+KeyNo+NewKey (EV1-Konvention) -- siehe
    // Funktionskommentar. NICHT CRC16 ueber nur NewKey (das war die
    // fruehere, falsche D40-Annahme).
    uint8_t header[18];
    header[0] = 0xC4;
    header[1] = keyNo;
    memcpy(header + 2, newKey16, 16);
    uint32_t crc = desfireCrc32(header, sizeof(header));
    memcpy(plain, newKey16, 16);
    plain[16] = (uint8_t)(crc & 0xFF);
    plain[17] = (uint8_t)((crc >> 8) & 0xFF);
    plain[18] = (uint8_t)((crc >> 16) & 0xFF);
    plain[19] = (uint8_t)((crc >> 24) & 0xFF);
    totalLen = 24; // 20 Nutzbyte auf 24 (3 DES-Bloecke) nullgepolstert
    desfireDes3Cbc(sessionKey16, iv, true, plain, cipher, totalLen);
  }

  uint8_t params[33];
  params[0] = keyNo;
  memcpy(params + 1, cipher, totalLen);
  uint8_t respBuf[16];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0xC4, params, 1 + totalLen, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00) {
    logMsg("desfireChangeKeySame(key %u): %s (status=0x%02X)\n", keyNo, desfireStatusName(status), status);
    return false;
  }
  return true;
}

// CreateApplication (0xCA): AID(3, LSB) + KeySettings1(1) + KeySettings2(1).
// KeySettings1=0x0F: jeder Schluessel nur mit sich selbst aenderbar (passt
// zum Auth-mit-Key-X-dann-ChangeKey-Key-X-Muster oben), Konfiguration
// danach nicht mehr aenderbar. KeySettings2: Bit7=1 falls AES verwendet
// werden soll (sonst (2K3)DES), untere 4 Bit = Anzahl Schluessel in der
// App (hier immer 1 -- wir nutzen ausschliesslich Key 0).
inline bool desfireCreateApplication(const uint8_t aid[3], bool aesKeys) {
  uint8_t params[5];
  memcpy(params, aid, 3);
  params[3] = 0x0F;
  params[4] = (aesKeys ? 0x80 : 0x00) | 0x01;
  uint8_t respBuf[8];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0xCA, params, 5, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00) {
    logMsg("desfireCreateApplication(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return false;
  }
  return true;
}

// DeleteApplication (0xDA): muss auf PICC-Ebene (AID 000000) authentifiziert
// aufgerufen werden, nicht innerhalb der zu loeschenden App.
inline bool desfireDeleteApplication(const uint8_t aid[3]) {
  uint8_t respBuf[8];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0xDA, aid, 3, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00) {
    logMsg("desfireDeleteApplication(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return false;
  }
  return true;
}

// CreateValueFile (0xCC): FileNo(1) + CommSettings(1) + AccessRights(2) +
// LowerLimit(4,LSB) + UpperLimit(4,LSB) + InitialValue(4,LSB) +
// LimitedCreditEnabled(1). CommSettings=0x00 (Plain) -- unverschluesselt,
// passt zum Rest dieser Datei (kein CMAC/Secure-Messaging implementiert).
// AccessRights=0x0000: Lesen/Schreiben/Lesen+Schreiben/Aendern alle mit
// Key 0. LowerLimit steuert, ob/wie tief das Guthaben durch Debit unter 0
// fallen kann -- die KARTE SELBST lehnt ein Debit unterhalb LowerLimit ab
// (siehe desfireDebit()), keine eigene Guthaben-Pruefung noetig.
inline bool desfireCreateValueFile(uint8_t fileNo, int32_t lowerLimit, int32_t upperLimit, int32_t initialValue) {
  uint8_t params[17];
  params[0] = fileNo;
  params[1] = 0x00;
  params[2] = 0x00; params[3] = 0x00;
  desfirePutLE32(params + 4, lowerLimit);
  desfirePutLE32(params + 8, upperLimit);
  desfirePutLE32(params + 12, initialValue);
  params[16] = 0x00;
  uint8_t respBuf[8];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0xCC, params, 17, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00) {
    logMsg("desfireCreateValueFile(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return false;
  }
  return true;
}

inline bool desfireCredit(uint8_t fileNo, int32_t amount) {
  uint8_t params[5];
  params[0] = fileNo;
  desfirePutLE32(params + 1, amount);
  uint8_t respBuf[8];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0x0C, params, 5, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00) {
    logMsg("desfireCredit(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return false;
  }
  return true;
}

// Debit (0xDC) -- die Karte lehnt selbst ab (typischerweise Status 0xBE
// BOUNDARY_ERROR), wenn der Abbuchungsbetrag das Guthaben unter
// LowerLimit (siehe desfireCreateValueFile()) druecken wuerde. Keine
// eigene Vorab-Pruefung noetig/implementiert.
inline bool desfireDebit(uint8_t fileNo, int32_t amount) {
  uint8_t params[5];
  params[0] = fileNo;
  desfirePutLE32(params + 1, amount);
  uint8_t respBuf[8];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0xDC, params, 5, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00) {
    logMsg("desfireDebit(): %s (status=0x%02X) -- vermutlich zu wenig Guthaben, falls BOUNDARY_ERROR.\n",
                  desfireStatusName(status), status);
    return false;
  }
  return true;
}

inline bool desfireGetValue(uint8_t fileNo, int32_t &valueOut) {
  uint8_t respBuf[8];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0x6C, &fileNo, 1, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00 || respLen < 4) {
    logMsg("desfireGetValue(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return false;
  }
  valueOut = desfireGetLE32(respBuf);
  return true;
}

// CommitTransaction (0xC7) -- OHNE das werden Credit/Debit beim naechsten
// Kommando bzw. beim Verlassen des RF-Feldes automatisch verworfen
// (DESFire-Transaktionsmodell). Immer direkt nach Credit/Debit aufrufen.
inline bool desfireCommitTransaction() {
  uint8_t respBuf[8];
  uint16_t respLen = 0;
  uint8_t status = desfireTransceive(0xC7, nullptr, 0, respBuf, &respLen, sizeof(respBuf));
  if (status != 0x00) {
    logMsg("desfireCommitTransaction(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return false;
  }
  return true;
}
