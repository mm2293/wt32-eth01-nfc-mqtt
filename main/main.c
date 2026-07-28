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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ethernet_setup.h"
#include "pn532_uart.h"
#include "relay_control.h"
#include "mqtt_client_setup.h"

static const char *TAG = "main";

#define MQTT_BROKER_URI  "mqtt://10.60.16.71:1883"
#define MQTT_USERNAME    "mqtt"
#define MQTT_PASSWORD    "mqtt-2025!"

// Die HomeKey reader_group_identifier kommt jetzt zur Laufzeit vom Addon
// (retained MQTT-Topic nfc/homekey_group_id, siehe mqtt_client_setup.c),
// sobald es per HAP mit der Home-App gepairt wurde. Bis dahin bleibt sie
// beim Default 00...00 (siehe pn532_uart.c) -- der ECP-Broadcast wird
// trotzdem gesendet, nur antwortet dann kein Geraet mit einem gueltigen
// Kryptogramm darauf.

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

        char uid_str[32] = {0};
        for (int i = 0; i < card.uid_len; i++) {
            sprintf(uid_str + (i * 2), "%02X", card.uid[i]);
        }
        ESP_LOGI(TAG, "Karte erkannt! UID=%s SAK=0x%02X ISO14443-4=%d Session=%" PRIu32,
                 uid_str, card.sak, card.iso14443_4, session_id);

        mqtt_client_setup_publish_card(card.uid, card.uid_len, card.sak, card.atqa,
                                        session_id, card.iso14443_4);

        if (card.iso14443_4) {
            // APDU-Relay-Schleife: auf weitere Kommandos vom Addon warten und
            // per InDataExchange an die noch selektierte Karte weiterreichen,
            // bis das Addon das Endergebnis meldet oder 3s nichts mehr kommt.
            while (1) {
                mqtt_apdu_cmd_t cmd;
                bool session_ended = false;
                if (!mqtt_client_setup_wait_apdu_cmd(session_id, &cmd, &session_ended, 3000)) {
                    if (!session_ended) {
                        ESP_LOGW(TAG, "Session %" PRIu32 ": Timeout, breche APDU-Relay ab", session_id);
                    }
                    break;
                }

                uint8_t resp[MQTT_APDU_MAX_LEN];
                size_t resp_len = 0;
                esp_err_t err = pn532_data_exchange(cmd.apdu, cmd.apdu_len, resp, sizeof(resp), &resp_len, 500);
                if (err == ESP_OK) {
                    mqtt_client_setup_publish_apdu_response(session_id, true, resp, resp_len, NULL);
                } else {
                    ESP_LOGW(TAG, "Session %" PRIu32 ": InDataExchange fehlgeschlagen (%s)", session_id, esp_err_to_name(err));
                    mqtt_client_setup_publish_apdu_response(session_id, false, NULL, 0, "pn532_data_exchange fehlgeschlagen");
                }
            }
        } else {
            // Reine UID-/Mifare-Classic-Karten brauchen kein APDU-Relay --
            // trotzdem kurz auf das Ergebnis warten, damit nicht sofort
            // weitergepollt (und das Relais evtl. verpasst) wird.
            mqtt_apdu_cmd_t cmd;
            bool session_ended = false;
            mqtt_client_setup_wait_apdu_cmd(session_id, &cmd, &session_ended, 2000);
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

    ESP_ERROR_CHECK(ethernet_setup_init());
    ESP_ERROR_CHECK(relay_control_init());

    ESP_LOGI(TAG, "Warte auf Ethernet-IP...");
    while (!ethernet_setup_has_ip()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t mqtt_err = mqtt_client_setup_init(MQTT_BROKER_URI, MQTT_USERNAME, MQTT_PASSWORD);
    if (mqtt_err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT-Client konnte nicht gestartet werden (Fehler %d)", mqtt_err);
    }

    xTaskCreate(pn532_init_task, "pn532_init", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initialisierung gestartet.");
}
