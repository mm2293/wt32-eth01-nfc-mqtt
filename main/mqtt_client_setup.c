/*
 * MQTT-Anbindung: Gegenstelle zum NFC-Zutritts-Addon (siehe mqtt_bridge.py).
 * Sendet erkannte UID/SAK/ATQA auf dem 'nfc/raw'-Topic, empfaengt das
 * Ergebnis auf 'nfc/result' und loest bei granted=true den Relais-Impuls aus.
 */

#include "mqtt_client_setup.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "relay_control.h"

static const char *TAG = "mqtt_client_setup";

#define TOPIC_INCOMING_RESULT  "nfc/result"
#define TOPIC_OUTGOING_RAW     "nfc/raw"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT verbunden, abonniere %s", TOPIC_INCOMING_RESULT);
            esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_INCOMING_RESULT, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT getrennt");
            break;

        case MQTT_EVENT_DATA: {
            // Ergebnis vom Addon empfangen: JSON parsen, bei granted=true Relais schalten
            char payload[256] = {0};
            int copy_len = event->data_len < sizeof(payload) - 1 ? event->data_len : sizeof(payload) - 1;
            memcpy(payload, event->data, copy_len);

            ESP_LOGI(TAG, "Ergebnis empfangen: %s", payload);

            cJSON *json = cJSON_Parse(payload);
            if (json != NULL) {
                cJSON *granted = cJSON_GetObjectItem(json, "granted");
                if (cJSON_IsTrue(granted)) {
                    ESP_LOGI(TAG, "Zutritt gewaehrt, schalte Relais");
                    relay_control_pulse();
                } else {
                    ESP_LOGI(TAG, "Zutritt verweigert");
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "Ergebnis-JSON konnte nicht geparst werden");
            }
            break;
        }

        default:
            break;
    }
}

esp_err_t mqtt_client_setup_init(const char *broker_uri, const char *username, const char *password)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = username,
        .credentials.authentication.password = password,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_mqtt_client);
}

void mqtt_client_setup_publish_card(const uint8_t *uid, uint8_t uid_len, uint8_t sak, const uint8_t atqa[2])
{
    if (s_mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT-Client noch nicht initialisiert, ueberspringe Publish");
        return;
    }

    char uid_hex[21] = {0};  // max 10 Byte UID -> 20 Hex-Zeichen + Nullterminierung
    for (int i = 0; i < uid_len && i < 10; i++) {
        snprintf(&uid_hex[i * 2], 3, "%02X", uid[i]);
    }

    char atqa_hex[5];
    snprintf(atqa_hex, sizeof(atqa_hex), "%02X%02X", atqa[0], atqa[1]);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "uid", uid_hex);
    cJSON_AddNumberToObject(json, "sak", sak);
    cJSON_AddStringToObject(json, "atqa", atqa_hex);

    char *payload = cJSON_PrintUnformatted(json);
    ESP_LOGI(TAG, "Sende Kartendaten: %s", payload);

    esp_mqtt_client_publish(s_mqtt_client, TOPIC_OUTGOING_RAW, payload, 0, 1, 0);

    free(payload);
    cJSON_Delete(json);
}

