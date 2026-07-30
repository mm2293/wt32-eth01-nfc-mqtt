/*
 * WT32-ETH01 NFC-Gateway: Hauptprogramm.
 *
 * Pollt kontinuierlich nach Karten (inkl. HomeKey-ECP-Broadcast zwischen den
 * Versuchen, siehe pn532_uart.c:pn532_poll_once()). Bei ISO14443-4-faehigen
 * Karten (DESFire, HomeKey) bleibt die Karte nach der Erkennung selektiert
 * und weitere APDUs werden per MQTT-Relay (nfc/apdu_cmd <-> nfc/apdu_resp)
 * mit dem Addon ausgetauscht, bis das Addon das Ergebnis (nfc/result)
 * veroeffentlicht.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ethernet_setup.h"
#include "pn532_uart.h"
#include "relay_control.h"
#include "mqtt_client_setup.h"
#include "app_config.h"
#include "web_config.h"

static const char *TAG = "main";

// Netzwerk/MQTT/Relais-Parameter kommen jetzt zur Laufzeit aus NVS (siehe
// app_config.h) statt aus Compile-Time-Konstanten -- ueber die Mini-WebGUI
// (web_config.c, erreichbar unter http://<geraet-ip>/ sobald Ethernet eine
// IP hat) aenderbar. Ein frisch geflashtes Geraet verhaelt sich dank der
// Defaults in app_config_load() unveraendert wie vorher.

// Die HomeKey reader_group_identifier kommt jetzt zur Laufzeit vom Addon
// (retained MQTT-Topic nfc/homekey_group_id, siehe mqtt_client_setup.c),
// sobald es per HAP mit der Home-App gepairt wurde. Bis dahin bleibt sie
// beim Default 00...00 (siehe pn532_uart.c) -- der ECP-Broadcast wird
// trotzdem gesendet, nur antwortet dann kein Geraet mit einem gueltigen
// Kryptogramm darauf.

// Ueber die WebGUI konfigurierbar (app_config_t.apdu_relay_timeout_ms), in
// app_main() aus der geladenen Konfiguration gesetzt, bevor card_event_task
// gestartet wird -- siehe deren Verwendung unten in der Kommando-Relay-
// Schleife.
static uint32_t s_apdu_relay_timeout_ms = 3000;

static void card_event_task(void *pvParameters)
{
    static uint32_t s_session_counter = 0;
    pn532_card_t card;

    while (1) {
        if (pn532_poll_once(&card, 300) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t session_id = ++s_session_counter;
        // Zeitbasis fuer die Latenz-Logs unten: misst, wie viel Zeit seit der
        // Kartenerkennung (RATS bereits erfolgt) vergeht, bis das erste/jedes
        // weitere APDU tatsaechlich per InDataExchange rausgeht -- HomeKey/
        // DESFire-Sitzungen koennen bei zu langer Inaktivitaet nach RATS
        // abbrechen (siehe pn532_uart.c:pn532_data_exchange() Kommentar), und
        // bislang fehlte die Sichtbarkeit, WELCHER Anteil (MQTT-Rundlauf vs.
        // Addon-Verarbeitung vs. RF-Austausch selbst) dafuer verantwortlich ist.
        int64_t t_detect_us = esp_timer_get_time();

        char uid_str[32] = {0};
        for (int i = 0; i < card.uid_len; i++) {
            sprintf(uid_str + (i * 2), "%02X", card.uid[i]);
        }
        ESP_LOGI(TAG, "Karte erkannt! UID=%s SAK=0x%02X ISO14443-4=%d Session=%" PRIu32,
                 uid_str, card.sak, card.iso14443_4, session_id);

        mqtt_client_setup_publish_card(card.uid, card.uid_len, card.sak, card.atqa,
                                        session_id, card.iso14443_4);

        // Kommando-Relay-Schleife: auf weitere Kommandos vom Addon warten und
        // per InDataExchange an die noch selektierte Karte weiterreichen, bis
        // das Addon das Endergebnis meldet oder 3s nichts mehr kommt.
        //
        // WICHTIG: laeuft UNABHAENGIG von card.iso14443_4 -- PN532s
        // InDataExchange transportiert ISO7816-APDUs (DESFire, HomeKey)
        // genauso wie native MIFARE-Classic-Kommandos (Auth 0x60/0x61, Read
        // 0x30, Write 0xA0, siehe mifare_classic_module.py), nur eben ohne
        // vorherige ATS/ISO14443-4-Aktivierung (siehe auch der analoge
        // Kommentar in mqtt_bridge.py:_process_card()). Frueher lief das
        // Relay nur bei iso14443_4-Karten, mit der (seit dem automatischen
        // Mifare-Classic-Crypto1-Anlernversuch falschen) Annahme "reine UID-/
        // Mifare-Classic-Karten brauchen kein Relay" -- dadurch ging fuer
        // Mifare Classic ueberhaupt nie ein Auth-/Read-/Write-Kommando an die
        // Karte raus, jeder Crypto1-Versuch lief lautlos in ein Timeout und
        // fiel automatisch (aber ohne echten Versuch) auf UID-Only zurueck.
        int apdu_index = 0;
        while (1) {
            mqtt_apdu_cmd_t cmd;
            bool session_ended = false;
            int64_t t_wait_start_us = esp_timer_get_time();
            if (!mqtt_client_setup_wait_apdu_cmd(session_id, &cmd, &session_ended, s_apdu_relay_timeout_ms)) {
                if (!session_ended) {
                    ESP_LOGW(TAG, "Session %" PRIu32 ": Timeout, breche Kommando-Relay ab", session_id);
                }
                break;
            }
            apdu_index++;
            int64_t t_cmd_received_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Session %" PRIu32 ": Kommando #%d empfangen (%lld ms seit Erkennung, "
                          "%lld ms Wartezeit auf diese Nachricht)",
                     session_id, apdu_index,
                     (long long)((t_cmd_received_us - t_detect_us) / 1000),
                     (long long)((t_cmd_received_us - t_wait_start_us) / 1000));

            uint8_t resp[MQTT_APDU_MAX_LEN];
            size_t resp_len = 0;
            // Aus dem FWI in der ATS der aktuellen Karte/des Geraets
            // berechnetes Timeout (siehe pn532_get_response_timeout_ms()) --
            // bei Karten ohne ATS (z.B. Mifare Classic) greift dessen
            // konservativer Default-Fallback.
            //
            // native=true fuer Mifare Classic (SAK 0x08/0x18, iso14443_4=
            // false): dort ist das PN532-Statusbyte selbst die eigentliche
            // Kartenantwort (z.B. 0x14 bei falschem Auth-Key -- normal, kein
            // Fehler) und muss unveraendert durchgereicht werden, statt als
            // Kommunikationsfehler behandelt zu werden (siehe
            // pn532_uart.c:pn532_data_exchange_ex()). Ohne das schlug JEDER
            // Mifare-Classic-Crypto1-Anlernversuch fehl, sobald die Karte
            // nicht mehr auf dem Werksstandard-Key war.
            int64_t t_exchange_start_us = esp_timer_get_time();
            esp_err_t err = pn532_data_exchange_ex(cmd.apdu, cmd.apdu_len, resp, sizeof(resp), &resp_len,
                                                    pn532_get_response_timeout_ms(), !card.iso14443_4);
            int64_t t_exchange_end_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Session %" PRIu32 ": InDataExchange #%d %s, dauerte %lld ms "
                          "(%lld ms seit Erkennung)",
                     session_id, apdu_index, err == ESP_OK ? "OK" : "FEHLGESCHLAGEN",
                     (long long)((t_exchange_end_us - t_exchange_start_us) / 1000),
                     (long long)((t_exchange_end_us - t_detect_us) / 1000));
            if (err == ESP_OK) {
                mqtt_client_setup_publish_apdu_response(session_id, true, resp, resp_len, NULL);
            } else {
                ESP_LOGW(TAG, "Session %" PRIu32 ": InDataExchange fehlgeschlagen (%s)", session_id, esp_err_to_name(err));
                mqtt_client_setup_publish_apdu_response(session_id, false, NULL, 0, "pn532_data_exchange fehlgeschlagen");
            }
        }

        pn532_release_field();
        vTaskDelay(pdMS_TO_TICKS(200));  // Entprellzeit
    }
}

static void pn532_init_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starte PN532-Initialisierung...");

    // 1. UART-Treiber initialisieren
    pn532_uart_init();

    // 2. SAM-Konfiguration durchführen
    ESP_LOGI(TAG, "Konfiguriere PN532 SAM...");
    while (pn532_sam_configuration() != ESP_OK) {
        ESP_LOGW(TAG, "PN532 antwortet nicht, erneuter Versuch in 3s...");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    ESP_LOGI(TAG, "PN532 erfolgreich initialisiert!");

    // 3. Event-Task starten
    // 6144 reichte fuer die alte MQTT_APDU_MAX_LEN=250; seit 2048 (fuer
    // InDataExchange-Fortsetzungen/HomeKey-ATTESTATION) liegen allein
    // mqtt_apdu_cmd_t cmd (~2064 Byte) und resp[MQTT_APDU_MAX_LEN] (2048 Byte)
    // gleichzeitig auf dem Stack dieser Task -- entsprechend vergroessert.
    xTaskCreate(card_event_task, "card_event", 12288, NULL, 5, NULL);

    // Init-Task beenden
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "WT32-ETH01 NFC-Gateway startet...");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    app_config_t cfg;
    app_config_load(&cfg);

    s_apdu_relay_timeout_ms = cfg.apdu_relay_timeout_ms;

    ESP_ERROR_CHECK(ethernet_setup_init(&cfg));
    ESP_ERROR_CHECK(relay_control_init(cfg.relay_pulse_ms));

    ESP_LOGI(TAG, "Warte auf Ethernet-IP...");
    while (!ethernet_setup_has_ip()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t web_err = web_config_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "Config-WebGUI konnte nicht gestartet werden (Fehler %d)", web_err);
    }

    esp_err_t mqtt_err = mqtt_client_setup_init(cfg.mqtt_broker_uri, cfg.mqtt_username, cfg.mqtt_password,
                                                 cfg.mqtt_client_id,
                                                 cfg.topic_raw, cfg.topic_apdu_cmd, cfg.topic_apdu_resp,
                                                 cfg.topic_result, cfg.topic_homekey_group_id,
                                                 cfg.relay_pulse_via_mqtt, cfg.topic_relay_pulse_ms);
    if (mqtt_err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT-Client konnte nicht gestartet werden (Fehler %d)", mqtt_err);
    }

    xTaskCreate(pn532_init_task, "pn532_init", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initialisierung gestartet.");
}
