#pragma once

/*
 * Automatischer Dictionary-Scan fuer MIFARE Classic: testet eine feste
 * Liste gaengiger Default-Keys (identisch zu mfocs eingebautem Key-Dictionary,
 * siehe PROTOCOL.md) gegen Key A und Key B jedes Sektors -- per direktem
 * PN532-Zugriff (pn532_data_exchange_ex, native=true), ohne MQTT-Rundlauf pro
 * einzelnem Versuch. Deckt NUR den Dictionary-Teil ab (also "ist die Karte
 * noch auf Standard-Keys"); ein echter Nested/Dark-Side-Cracking-Angriff wie
 * bei mfoc fuer Karten mit individuellen Keys ist damit NICHT moeglich (der
 * PN532 kapselt die Crypto1-Authentifizierung intern als Blackbox und legt
 * dafuer noetige rohe Zwischenzustaende nicht offen).
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// 40 reicht fuer eine 4K-Karte (32 Sektoren a 4 Bloecke + 8 Sektoren a 16
// Bloecke); 1K-Karten nutzen nur die ersten 16.
#define MIFARE_SCAN_MAX_SECTORS 40

typedef struct {
    bool key_a_found;
    uint8_t key_a[6];
    bool key_b_found;
    uint8_t key_b[6];
} mifare_sector_keys_t;

typedef struct {
    uint8_t sector_count;
    mifare_sector_keys_t sectors[MIFARE_SCAN_MAX_SECTORS];
} mifare_scan_result_t;

/* uid/uid_len wie von pn532_poll_once() geliefert (MIFARE Classic: immer
 * 4 Byte). sak bestimmt die Sektoranzahl: 0x18 -> 40 Sektoren (4K), sonst
 * 16 Sektoren (1K/Mini -- bei unbekanntem SAK mit Log-Warnung als 1K
 * angenommen). Blockierend, kann je nach Kartenzustand mehrere Sekunden bis
 * niedrige Minuten dauern (worst case: kein Sektor auf einem Default-Key,
 * dann werden alle Kombinationen durchprobiert). */
esp_err_t mifare_classic_scan_default_keys(const uint8_t *uid, uint8_t uid_len, uint8_t sak,
                                             mifare_scan_result_t *out_result);
