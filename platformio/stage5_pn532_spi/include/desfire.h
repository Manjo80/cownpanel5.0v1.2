#pragma once
// Native DESFire-Kommandos ueber den PN532 (kein ISO7816-Wrapping -- reine
// Rohkommandos via Adafruit_PN532::inDataExchange(), das die aktuell per
// inListPassiveTarget() aktivierte Karte anspricht).
//
// AUSDRUECKLICH BEGRENZTER UMFANG (bewusste Entscheidung, siehe
// firmware/README.md):
//  - Unterstuetzt Legacy-2K3DES- (Kommando 0x0A) und AES-128- (Kommando
//    0xAA) Authentifizierung mit einem 16-Byte-Schluessel (Default-Schluessel
//    = 16 Nullbytes). KEIN 3K3DES, KEIN EV2-Secure-Messaging.
//  - Session-Key wird aus RndA/RndB abgeleitet, aber NICHT fuer CMAC-
//    Pruefung oder Enciphered-Kommunikation genutzt -- Dateien im
//    Enciphered-Modus werden ERKANNT, aber nicht entschluesselt (siehe
//    desfireGetFileSettings()/commMode). MACed-Dateien werden gelesen,
//    ihr MAC/CMAC wird NICHT geprueft (Daten selbst sind bei MACed-Modus
//    ohnehin unverschluesselt uebertragen, nur zusaetzlich signiert).
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

#include <Arduino.h>
#include <Adafruit_PN532.h>
#include <mbedtls/des.h>
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
  uint8_t sendBuf[16];
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
    Serial.printf("desfireGetVersion(): %s (status=0x%02X, len=%u)\n", desfireStatusName(status), status, len);
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
    Serial.printf("desfireGetApplicationIDs(): %s (status=0x%02X)\n", desfireStatusName(status), status);
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
    Serial.printf("desfireSelectApplication(): %s (status=0x%02X)\n", desfireStatusName(status), status);
  }
  return status == 0x00;
}

inline uint8_t desfireGetFileIDs(uint8_t fileIds[], uint8_t maxFiles) {
  uint8_t buf[32];
  uint16_t len = 0;
  uint8_t status = desfireTransceive(0x6F, nullptr, 0, buf, &len, sizeof(buf));
  if (status != 0x00) {
    Serial.printf("desfireGetFileIDs(): %s (status=0x%02X)\n", desfireStatusName(status), status);
    return 0;
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
    Serial.printf("desfireGetFileSettings(file %u): %s (status=0x%02X)\n", fileId, desfireStatusName(status), status);
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
    Serial.printf("desfireReadData(file %u): %s (status=0x%02X)\n", fileId, desfireStatusName(status), status);
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
    Serial.printf("desfireReadRecords(file %u): %s (status=0x%02X)\n", fileId, desfireStatusName(status), status);
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

// 2K3DES-CBC (mit K1=K2=key16[0..7], funktional identisch zu Single-DES
// fuer den haeufigen Fall eines 8-Byte-Werksschluessels, der hier auf 16
// Byte dupliziert wird -- siehe Aufrufer). IV wird NICHT ueber mehrere
// Aufrufe verkettet, das macht die Authentifizierungslogik unten selbst
// (jeder Aufruf bekommt seinen IV explizit uebergeben).
inline void desfireDes3Cbc(const uint8_t key16[16], const uint8_t iv[8], bool encrypt,
                            const uint8_t *in, uint8_t *out, size_t len) {
  mbedtls_des3_context ctx;
  mbedtls_des3_init(&ctx);
  if (encrypt) mbedtls_des3_set2key_enc(&ctx, key16);
  else mbedtls_des3_set2key_dec(&ctx, key16);
  uint8_t ivBuf[8];
  memcpy(ivBuf, iv, 8);
  mbedtls_des3_crypt_cbc(&ctx, encrypt ? MBEDTLS_DES_ENCRYPT : MBEDTLS_DES_DECRYPT, len, ivBuf, in, out);
  mbedtls_des3_free(&ctx);
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

// Legacy-2K3DES-Authentifizierung (Kommando 0x0A), ISO/IEC-9798-2-3-Pass-
// Mutual-Authentication. key16 = 16-Byte-Schluessel (Werks-Default:
// 16 Nullbytes). sessionKeyOut wird bei Erfolg befuellt (hier ungenutzt,
// da keine Secure-Messaging-Kommandos folgen -- siehe Datei-Kopfkommentar).
inline bool desfireAuthDes3(uint8_t keyNo, const uint8_t key16[16], uint8_t sessionKeyOut[16]) {
  uint8_t send1[2] = { 0x0A, keyNo };
  uint8_t resp[32];
  uint8_t respLen = sizeof(resp);
  if (!nfc.inDataExchange(send1, 2, resp, &respLen) || respLen < 9 || resp[0] != 0xAF) {
    Serial.printf("desfireAuthDes3: Schritt 1 unerwartet (status=0x%02X, len=%u)\n",
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
    Serial.printf("desfireAuthDes3: Schritt 2 unerwartet (status=0x%02X, len=%u) -- "
                  "vermutlich falscher Schluessel oder Karte hat Werksschluessel nicht mehr.\n",
                  respLen > 0 ? resp[0] : 0xFF, respLen);
    return false;
  }
  uint8_t encRndAResp[8];
  memcpy(encRndAResp, resp + 1, 8);

  uint8_t rndARot[8];
  desfireDes3Cbc(key16, encAB + 8, false, encRndAResp, rndARot, 8); // IV = letzter von uns gesendeter Block

  uint8_t rndAExpectedRot[8];
  memcpy(rndAExpectedRot, rndA, 8);
  desfireRotateLeft1(rndAExpectedRot, 8);

  if (memcmp(rndARot, rndAExpectedRot, 8) != 0) {
    Serial.println("desfireAuthDes3: RndA-Rueckpruefung fehlgeschlagen (falscher Schluessel?).");
    return false;
  }

  // Sitzungsschluessel (2K3DES): RndA[0:4] + RndB[0:4] + RndA[4:8] + RndB[4:8]
  memcpy(sessionKeyOut, rndA, 4);
  memcpy(sessionKeyOut + 4, rndB, 4);
  memcpy(sessionKeyOut + 8, rndA + 4, 4);
  memcpy(sessionKeyOut + 12, rndB + 4, 4);
  return true;
}

// AES-128-Authentifizierung (Kommando 0xAA), gleiche Grundstruktur wie
// desfireAuthDes3(), nur mit 16-Byte-Bloecken statt 8-Byte-Bloecken.
inline bool desfireAuthAes(uint8_t keyNo, const uint8_t key16[16], uint8_t sessionKeyOut[16]) {
  uint8_t send1[2] = { 0xAA, keyNo };
  uint8_t resp[40];
  uint8_t respLen = sizeof(resp);
  if (!nfc.inDataExchange(send1, 2, resp, &respLen) || respLen < 17 || resp[0] != 0xAF) {
    Serial.printf("desfireAuthAes: Schritt 1 unerwartet (status=0x%02X, len=%u)\n",
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
    Serial.printf("desfireAuthAes: Schritt 2 unerwartet (status=0x%02X, len=%u) -- "
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

  if (memcmp(rndARot, rndAExpectedRot, 16) != 0) {
    Serial.println("desfireAuthAes: RndA-Rueckpruefung fehlgeschlagen (falscher Schluessel?).");
    return false;
  }

  // Sitzungsschluessel (AES): RndA[0:4] + RndB[0:4] + RndA[12:16] + RndB[12:16]
  memcpy(sessionKeyOut, rndA, 4);
  memcpy(sessionKeyOut + 4, rndB, 4);
  memcpy(sessionKeyOut + 8, rndA + 12, 4);
  memcpy(sessionKeyOut + 12, rndB + 12, 4);
  return true;
}

// Probiert Legacy-2K3DES zuerst (haeufiger bei DESFire EV0), dann AES
// (haeufiger bei DESFire EV1+), jeweils mit einem 16-Byte-Nullschluessel
// (Werks-Default). Gibt bei Erfolg true zurueck und setzt cipherNameOut
// auf "2K3DES" oder "AES".
inline bool desfireAuthDefaultKey(uint8_t keyNo, uint8_t sessionKeyOut[16], const char **cipherNameOut) {
  static const uint8_t zeroKey[16] = {0};
  if (desfireAuthDes3(keyNo, zeroKey, sessionKeyOut)) {
    *cipherNameOut = "2K3DES";
    return true;
  }
  if (desfireAuthAes(keyNo, zeroKey, sessionKeyOut)) {
    *cipherNameOut = "AES";
    return true;
  }
  *cipherNameOut = "keiner";
  return false;
}
