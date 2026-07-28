#ifndef PN532_UART_H
#define PN532_UART_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t atqa[2];
    uint8_t target_number;  // PN532 "Tg" (fuer InDataExchange), i.d.R. 1
    bool iso14443_4;        // SAK & 0x20 -- Karte unterstuetzt ISO-DEP/APDUs
} pn532_card_t;

esp_err_t pn532_uart_init(void);
esp_err_t pn532_sam_configuration(void);

/* Setzt die 8-Byte HomeKey Reader-Group-Identifier, die im ECP-Broadcast-Frame
 * gesendet wird (siehe homekey_lib/util/ecp.py:ECP.home() im Addon). Solange
 * das Addon noch nicht ueber HAP gepairt ist (siehe Schritt 2), bleibt dies
 * 00...00 -- ein iPhone mit bereits eingerichtetem Home Key wird darauf
 * schlicht nicht mit einem gueltigen Kryptogramm antworten. */
void pn532_set_homekey_group_identifier(const uint8_t identifier[8]);

/* Ein Poll-Zyklus:
 *  - Versucht zuerst InListPassiveTarget (findet normale Karten: UID-Tags,
 *    Mifare Classic, DESFire, ...).
 *  - Wird dabei NICHTS gefunden, sendet sie stattdessen einen ECP-Broadcast-
 *    Frame (InCommunicateThru), damit ein wartendes iPhone/eine Watch mit
 *    HomeKey aufwacht und im naechsten Zyklus auf InListPassiveTarget
 *    antwortet (siehe kormax/apple-home-key-reader util/bfclf.py:sense(),
 *    dessen Poll-Reihenfolge hier eins zu eins nachgebildet wird).
 *
 * Rueckgabe ESP_OK + befuellter out_card, wenn ein Target gefunden wurde
 * (das Target bleibt bis zum naechsten pn532_poll_once()/pn532_release_field()
 * selektiert und kann per pn532_data_exchange() weiter angesprochen werden).
 * ESP_ERR_NOT_FOUND, wenn in diesem Zyklus nichts gefunden wurde (dann
 * einfach direkt erneut aufrufen). */
esp_err_t pn532_poll_once(pn532_card_t *out_card, uint32_t timeout_ms);

/* Tauscht ein rohes ISO7816-APDU (bzw. beliebige Rohdaten) mit dem zuletzt
 * gefundenen Target aus. Nur gueltig direkt nach einem erfolgreichen
 * pn532_poll_once() mit iso14443_4=true, solange die Karte noch im Feld ist.
 * HINWEIS: Unterstuetzt keine PN532-Extended-Length-Frames -- APDU und
 * Antwort muessen zusammen mit Overhead unter ~250 Byte bleiben (reicht fuer
 * FAST/STANDARD-Auth; die HomeKey-ATTESTATION-Flow-Envelopes koennen das
 * ueberschreiten und sind damit hier noch nicht abgedeckt). */
esp_err_t pn532_data_exchange(const uint8_t *apdu, size_t apdu_len,
                               uint8_t *resp, size_t resp_cap, size_t *resp_len,
                               uint32_t timeout_ms);

/* Deaktiviert das RF-Feld (z.B. nach Sessionende/-abbruch), damit der
 * naechste pn532_poll_once() sauber neu beginnt. */
void pn532_release_field(void);

#endif
