#include "mifare_classic_scan.h"

#include <string.h>
#include "esp_log.h"

#include "pn532_uart.h"

static const char *TAG = "mifare_classic_scan";

// Identische Liste wie mfocs eingebautes Default-Key-Dictionary (siehe
// libnfc/mfoc) -- auf echter Hardware bereits gegen eine Testkarte
// verifiziert (siehe PROTOCOL.md fuer den konkreten Fall, der zur
// Reselektions-Korrektur in pn532_uart.c gefuehrt hat).
static const uint8_t DEFAULT_KEYS[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97},
    {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F},
    {0xA0, 0x47, 0x8C, 0xC3, 0x90, 0x91},
    {0x53, 0x3C, 0xB6, 0xC7, 0x23, 0xF6},
    {0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9},
};
#define DEFAULT_KEY_COUNT (sizeof(DEFAULT_KEYS) / sizeof(DEFAULT_KEYS[0]))

// Sector-Trailer-Block: Sektoren 0-31 haben 4 Bloecke (Trailer = Sektor*4+3),
// Sektoren 32-39 (nur 4K) haben 16 Bloecke (Trailer entsprechend weiter hinten).
static uint16_t sector_trailer_block(uint8_t sector)
{
    if (sector < 32) {
        return (uint16_t)(sector * 4 + 3);
    }
    return (uint16_t)(128 + (sector - 32) * 16 + 15);
}

// Ein einzelner Auth-Versuch. Nutzt pn532_data_exchange_ex() direkt (nicht
// den MQTT-Umweg) -- dieselbe Funktion, die auch main.c fuer per MQTT
// weitergereichte APDUs/native Kommandos verwendet, inkl. der Reselektion
// vor jedem Auth-Kommando (siehe pn532_uart.c).
static bool try_auth(uint8_t key_cmd, uint16_t block, const uint8_t *key,
                      const uint8_t *uid, uint8_t uid_len)
{
    if (uid_len != 4) {
        return false;  // MIFARE Classic hat immer eine 4-Byte-UID
    }

    uint8_t cmd[1 + 1 + 6 + 4];
    cmd[0] = key_cmd;
    cmd[1] = (uint8_t)block;
    memcpy(&cmd[2], key, 6);
    memcpy(&cmd[8], uid, 4);

    uint8_t resp[1];
    size_t resp_len = 0;
    esp_err_t err = pn532_data_exchange_ex(cmd, sizeof(cmd), resp, sizeof(resp), &resp_len, 500, true);
    if (err != ESP_OK || resp_len < 1) {
        return false;
    }
    // Siehe pn532_data_exchange_once_native(): letztes Antwort-Byte ist das
    // MIFARE-Statusbyte, 0x00 = Auth erfolgreich.
    return resp[resp_len - 1] == 0x00;
}

esp_err_t mifare_classic_scan_default_keys(const uint8_t *uid, uint8_t uid_len, uint8_t sak,
                                             mifare_scan_result_t *out_result)
{
    memset(out_result, 0, sizeof(*out_result));

    uint8_t sector_count;
    if (sak == 0x18) {
        sector_count = 40;  // MIFARE Classic 4K
    } else {
        if (sak != 0x08 && sak != 0x09 && sak != 0x28) {
            ESP_LOGW(TAG, "Unbekannter SAK 0x%02X fuer MIFARE-Classic-Scan, nehme 16 Sektoren (1K) an", sak);
        }
        sector_count = 16;  // MIFARE Classic 1K/Mini
    }
    out_result->sector_count = sector_count;

    ESP_LOGI(TAG, "Starte Dictionary-Scan: %u Sektoren, %u Keys je Sektor/Typ",
             sector_count, (unsigned)DEFAULT_KEY_COUNT);

    for (uint8_t sector = 0; sector < sector_count; sector++) {
        uint16_t block = sector_trailer_block(sector);
        mifare_sector_keys_t *s = &out_result->sectors[sector];

        for (size_t k = 0; k < DEFAULT_KEY_COUNT && !s->key_a_found; k++) {
            if (try_auth(0x60, block, DEFAULT_KEYS[k], uid, uid_len)) {
                s->key_a_found = true;
                memcpy(s->key_a, DEFAULT_KEYS[k], 6);
            }
        }
        for (size_t k = 0; k < DEFAULT_KEY_COUNT && !s->key_b_found; k++) {
            if (try_auth(0x61, block, DEFAULT_KEYS[k], uid, uid_len)) {
                s->key_b_found = true;
                memcpy(s->key_b, DEFAULT_KEYS[k], 6);
            }
        }

        ESP_LOGI(TAG, "Sektor %u (Block %u): Key A %s, Key B %s", sector, block,
                 s->key_a_found ? "gefunden" : "unbekannt",
                 s->key_b_found ? "gefunden" : "unbekannt");
    }

    ESP_LOGI(TAG, "Dictionary-Scan abgeschlossen");
    return ESP_OK;
}
