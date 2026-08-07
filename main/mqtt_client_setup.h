#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

// War 250 (ein einzelner PN532-Kurzframe): jetzt gross genug fuer per
// InDataExchange-Fortsetzung ("more data"-Bit) zusammengesetzte Antworten
// (z.B. HomeKey-ATTESTATION-Envelopes), siehe pn532_uart.c:pn532_data_exchange_once().
#define MQTT_APDU_MAX_LEN 2048

typedef struct {
    uint32_t session_id;
    uint8_t apdu[MQTT_APDU_MAX_LEN];
    size_t apdu_len;
} mqtt_apdu_cmd_t;

/* Initialisiert den MQTT-Client anhand der zur Laufzeit aus NVS geladenen
 * Konfiguration (siehe app_config.h) -- Topic-Namen, QoS/Retain je Topic,
 * Clean-Session-Verhalten, Zugangsdaten. cfg wird nur fuer den Aufbau der
 * Konfiguration gelesen, nicht referenziert (Werte werden kopiert bzw. an
 * esp_mqtt_client_init() uebergeben, das seinerseits intern kopiert).
 *
 * cfg->relay_pulse_via_mqtt: wenn true, wird zusaetzlich
 * cfg->topic_relay_pulse_ms (retained, Payload = Millisekunden als
 * Zahl-String) abonniert und jede empfangene Nachricht per
 * relay_control_set_pulse_ms() uebernommen (siehe PROTOCOL.md). Wenn false,
 * wird dieses Topic gar nicht erst abonniert und die feste, ueber die
 * WebGUI konfigurierte Pulsdauer bleibt massgeblich.
 *
 * cfg->pn532_raw_bridge_mode: steuert nur, ob zusaetzlich
 * nfc/apdu_relay_timeout_ms abonniert wird -- dieses Topic wirkt sich
 * einzig auf main.c:card_event_task() aus, die im Raw-Bridge-Modus nie
 * laeuft (siehe PROTOCOL.md). nfc/result bleibt in BEIDEN Modi abonniert,
 * da die Relaissteuerung unabhaengig vom PN532-Modus daran haengt. */
esp_err_t mqtt_client_setup_init(const app_config_t *cfg);

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

/* Meldet den Reedkontakt-Status (Tuer-/Schlossstatus, siehe reed_contact.c)
 * auf dem konfigurierten Topic (retained/QoS wie in app_config_t
 * eingestellt). closed=true -> Payload "closed", sonst "open". */
void mqtt_client_setup_publish_reed_state(bool closed);

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

/* Aktuell geltender Timeout (ms) fuer die APDU-Relay-Schleife in
 * main.c:card_event_task() -- Standard 3000ms, vom Addon zur Laufzeit per
 * retained MQTT-Topic (nfc/apdu_relay_timeout_ms, siehe PROTOCOL.md)
 * ueberschreibbar, z.B. um beim Oeffnen der interaktiven NFC-Shell
 * voruebergehend einen deutlich hoeheren Wert zu setzen. Wird bei jedem
 * Schleifendurchlauf frisch abgefragt, eine Aenderung wirkt sich also sofort
 * auf die naechste Wartezeit aus, kein Neustart noetig. */
uint32_t mqtt_client_setup_get_apdu_relay_timeout_ms(void);
