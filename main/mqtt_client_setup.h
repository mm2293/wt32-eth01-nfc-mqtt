#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// War 250 (ein einzelner PN532-Kurzframe): jetzt gross genug fuer per
// InDataExchange-Fortsetzung ("more data"-Bit) zusammengesetzte Antworten
// (z.B. HomeKey-ATTESTATION-Envelopes), siehe pn532_uart.c:pn532_data_exchange_once().
#define MQTT_APDU_MAX_LEN 2048

typedef struct {
    uint32_t session_id;
    uint8_t apdu[MQTT_APDU_MAX_LEN];
    size_t apdu_len;
} mqtt_apdu_cmd_t;

esp_err_t mqtt_client_setup_init(const char *broker_uri, const char *username, const char *password);

/* Meldet eine neu erkannte Karte. session_id identifiziert diesen
 * Kartenvorgang eindeutig gegenueber dem Addon (siehe nfc/apdu_cmd /
 * nfc/apdu_resp). iso14443_4 sagt dem Addon, ob ueberhaupt APDUs moeglich
 * sind (sonst reicht die reine UID). */
void mqtt_client_setup_publish_card(const uint8_t *uid, uint8_t uid_len, uint8_t sak,
                                     const uint8_t atqa[2], uint32_t session_id, bool iso14443_4);

/* Antwort auf ein per nfc/apdu_cmd empfangenes APDU. */
void mqtt_client_setup_publish_apdu_response(uint32_t session_id, bool ok,
                                              const uint8_t *resp, size_t resp_len,
                                              const char *error);

/* Blockiert bis zu timeout_ms auf das naechste APDU-Kommando ODER das
 * Sessionende (nfc/result) fuer die angegebene session_id; Nachrichten mit
 * abweichender session_id werden intern verworfen (nicht relevant, aber
 * verbrauchen nicht die volle Wartezeit erneut).
 * Rueckgabe true + out_cmd befuellt: naechstes APDU-Kommando.
 * Rueckgabe false + *out_session_ended=true: nfc/result fuer diese Session
 * wurde empfangen (regulaeres Ende).
 * Rueckgabe false + *out_session_ended=false: Timeout ohne Nachricht. */
bool mqtt_client_setup_wait_apdu_cmd(uint32_t session_id, mqtt_apdu_cmd_t *out_cmd,
                                      bool *out_session_ended, uint32_t timeout_ms);
