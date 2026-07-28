/*
 * WT32-ETH01 NFC-Gateway: Hauptprogramm mit direkter UART-Pufferabfrage.
 */

#include <stdio.h>
#include <string.h>
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

static void card_event_task(void *pvParameters)
{
    pn532_card_t card;

    while (1) {
        // 1. InAutoPoll starten
        if (pn532_start_auto_poll() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 2. Bis zu 2 Sekunden auf Erst-Byte warten
        if (pn532_read_auto_poll_response(&card, 2000) == ESP_OK) {
            ESP_LOGI(TAG, "Karte erkannt! SAK=0x%02X, UID Len=%d", card.sak, card.uid_len);

            char uid_str[32] = {0};
            for (int i = 0; i < card.uid_len; i++) {
                sprintf(uid_str + (i * 2), "%02X", card.uid[i]);
            }
            ESP_LOGI(TAG, "UID: %s", uid_str);

            mqtt_client_setup_publish_card(card.uid, card.uid_len, card.sak, card.atqa);

            // Entprellzeit (1s), damit beim Anlegen nicht doppelt gefeuert wird
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            // Nach Timeout kurz pausieren und erneut InAutoPoll aufrufen
            vTaskDelay(pdMS_TO_TICKS(10));
        }
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
    xTaskCreate(card_event_task, "card_event", 4096, NULL, 5, NULL);

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
