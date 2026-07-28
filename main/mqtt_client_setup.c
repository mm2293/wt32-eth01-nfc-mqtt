/*
 * MQTT-Anbindung: Gegenstelle zum NFC-Zutritts-Addon (siehe mqtt_bridge.py).
 *
 * Topics:
 *   nfc/raw        (ESP32 -> Addon)  Kartendaten inkl. session_id + iso14443_4
 *   nfc/apdu_cmd   (Addon -> ESP32)  {"session_id":N,"apdu_hex":"..."}
 *   nfc/apdu_resp  (ESP32 -> Addon)  {"session_id":N,"ok":true,"response_hex":"..."}
 *   nfc/result     (Addon -> ESP32)  bestehendes Format, beendet zusaetzlich
 *                                    die APDU-Relay-Session (siehe
 *                                    mqtt_client_setup_wait_apdu_cmd)
 *   nfc/homekey_group_id (Addon -> ESP32, retained) 8-Byte reader_group_identifier
 *                                    als Hex-String, siehe ha-nfc-addon/
 *                                    main.py:_on_homekey_reader_key_changed().
 *                                    Wird direkt an pn532_set_homekey_group_identifier()
 *                                    weitergereicht (siehe pn532_uart.c fuer den
 *                                    ECP-Broadcast, der diese ID enthaelt). Solange
 *                                    das Addon noch nicht per HAP gepairt ist, kommt
 *                                    hier nichts an und es bleibt beim Default 00...00.
 */

#include "mqtt_client_setup.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "relay_control.h"
#include "pn532_uart.h"

static const char *TAG = "mqtt_client_setup";

#define TOPIC_INCOMING_RESULT     "nfc/result"
#define TOPIC_OUTGOING_RAW        "nfc/raw"
#define TOPIC_INCOMING_APDU_CMD   "nfc/apdu_cmd"
#define TOPIC_OUTGOING_APDU_RESP  "nfc/apdu_resp"
#define TOPIC_INCOMING_HOMEKEY_GROUP_ID "nfc/homekey_group_id"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static QueueHandle_t s_session_queue = NULL;

typedef struct {
    bool is_session_end;
    uint32_t session_id;
    uint8_t apdu[MQTT_APDU_MAX_LEN];
    size_t apdu_len;
} session_queue_item_t;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex_decode(const char *hex, uint8_t *out, size_t out_cap)
{
    if (hex == NULL) return 0;
    size_t hex_len = strlen(hex);
    size_t n = hex_len / 2;
    if (n > out_cap) n = out_cap;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static void handle_apdu_cmd_message(const char *payload)
{
    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        ESP_LOGE(TAG, "apdu_cmd-JSON konnte nicht geparst werden");
        return;
    }

    cJSON *session_id_item = cJSON_GetObjectItem(json, "session_id");
    cJSON *apdu_hex_item = cJSON_GetObjectItem(json, "apdu_hex");

    if (!cJSON_IsNumber(session_id_item) || !cJSON_IsString(apdu_hex_item)) {
        ESP_LOGW(TAG, "apdu_cmd ohne session_id/apdu_hex ignoriert");
        cJSON_Delete(json);
        return;
    }

    // Heap statt Stack: session_queue_item_t enthaelt seit MQTT_APDU_MAX_LEN=2048
    // ein ~2KB grosses apdu[]-Feld. Diese Funktion laeuft im internen
    // Event-Handler-Task von esp-mqtt, dessen Stackgroesse hier nicht
    // konfiguriert/verifiziert ist -- ein weiterer Stack-Local dieser Groesse
    // waere ein Stack-Overflow-Risiko.
    session_queue_item_t *item = malloc(sizeof(session_queue_item_t));
    if (item == NULL) {
        ESP_LOGE(TAG, "apdu_cmd: kein Speicher fuer session_queue_item_t");
        cJSON_Delete(json);
        return;
    }
    memset(item, 0, sizeof(*item));
    item->is_session_end = false;
    item->session_id = (uint32_t)session_id_item->valuedouble;
    item->apdu_len = hex_decode(apdu_hex_item->valuestring, item->apdu, sizeof(item->apdu));

    cJSON_Delete(json);

    if (item->apdu_len == 0) {
        ESP_LOGW(TAG, "apdu_cmd mit leerem/ungueltigem apdu_hex ignoriert");
        free(item);
        return;
    }

    if (s_session_queue != NULL) {
        // xQueueSend kopiert den Inhalt in den internen Queue-Speicher (auf
        // dem Heap, von xQueueCreate angelegt) -- item kann danach sofort
        // freigegeben werden.
        xQueueSend(s_session_queue, item, 0);
    }
    free(item);
}

static void handle_homekey_group_id_message(const char *payload)
{
    uint8_t identifier[8];
    size_t len = hex_decode(payload, identifier, sizeof(identifier));
    if (len != sizeof(identifier)) {
        ESP_LOGW(TAG, "homekey_group_id mit ungueltiger Laenge ignoriert: %s", payload);
        return;
    }
    pn532_set_homekey_group_identifier(identifier);
    ESP_LOGI(TAG, "HomeKey reader_group_identifier aktualisiert: %s", payload);
}

static void handle_result_message(const char *payload)
{
    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        ESP_LOGE(TAG, "Ergebnis-JSON konnte nicht geparst werden");
        return;
    }

    cJSON *granted = cJSON_GetObjectItem(json, "granted");
    if (cJSON_IsTrue(granted)) {
        ESP_LOGI(TAG, "Zutritt gewaehrt, schalte Relais");
        relay_control_pulse();
    } else {
        ESP_LOGI(TAG, "Zutritt verweigert");
    }

    // Heap statt Stack, siehe Kommentar in handle_apdu_cmd_message() --
    // gleicher Grund (session_queue_item_t ist seit MQTT_APDU_MAX_LEN=2048
    // zu gross fuer den unklar dimensionierten esp-mqtt-Event-Handler-Stack).
    session_queue_item_t *item = malloc(sizeof(session_queue_item_t));
    if (item == NULL) {
        ESP_LOGE(TAG, "result: kein Speicher fuer session_queue_item_t");
        cJSON_Delete(json);
        return;
    }
    memset(item, 0, sizeof(*item));
    item->is_session_end = true;
    cJSON *session_id_item = cJSON_GetObjectItem(json, "session_id");
    item->session_id = cJSON_IsNumber(session_id_item) ? (uint32_t)session_id_item->valuedouble : 0;

    cJSON_Delete(json);

    if (s_session_queue != NULL) {
        xQueueSend(s_session_queue, item, 0);
    }
    free(item);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT verbunden, abonniere %s, %s und %s",
                     TOPIC_INCOMING_RESULT, TOPIC_INCOMING_APDU_CMD, TOPIC_INCOMING_HOMEKEY_GROUP_ID);
            esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_INCOMING_RESULT, 1);
            // QoS 0 bewusst: nfc/apdu_cmd wird pro Kartenvorgang mehrfach
            // (einmal je APDU) durchgereicht, oft innerhalb eines engen
            // Zeitfensters, das Karte/Handy fuer die laufende NFC-Transaktion
            // offenhalten. QoS 1 wuerde auf JEDEM Hop (Broker<->Client) eine
            // zusaetzliche PUBACK-Bestaetigung erzwingen -- Latenz, die hier
            // nichts bringt, da ein verlorenes APDU ohnehin ueber das eigene
            // Timeout auf Addon-Seite (mqtt_bridge.py) erkannt wird, nicht
            // durch MQTT-Zustellgarantien.
            esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_INCOMING_APDU_CMD, 0);
            // Retained: liefert beim (Wieder-)Verbinden sofort den zuletzt vom
            // Addon veroeffentlichten Wert nach, auch nach einem ESP32-Reboot.
            esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_INCOMING_HOMEKEY_GROUP_ID, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT getrennt");
            break;

        case MQTT_EVENT_DATA: {
            // Heap statt Stack: ein apdu_cmd-JSON kann bei
            // MQTT_APDU_MAX_LEN=2048 bis zu ~4100 Byte gross werden
            // (hex-kodiertes APDU + JSON-Huelle). Dieser Handler laeuft im
            // internen esp-mqtt-Event-Handler-Task, dessen Stackgroesse hier
            // nicht konfiguriert/verifiziert ist -- ein so grosser
            // Stack-Local waere ein Stack-Overflow-Risiko.
            size_t payload_cap = MQTT_APDU_MAX_LEN * 2 + 256;
            char *payload = malloc(payload_cap);
            if (payload == NULL) {
                ESP_LOGE(TAG, "MQTT_EVENT_DATA: kein Speicher fuer Payload-Puffer");
                break;
            }
            int copy_len = event->data_len < (int)payload_cap - 1 ? event->data_len : (int)payload_cap - 1;
            memcpy(payload, event->data, copy_len);
            payload[copy_len] = '\0';

            char topic[64] = {0};
            int topic_copy_len = event->topic_len < (int)sizeof(topic) - 1 ? event->topic_len : (int)sizeof(topic) - 1;
            memcpy(topic, event->topic, topic_copy_len);

            if (strcmp(topic, TOPIC_INCOMING_RESULT) == 0) {
                ESP_LOGI(TAG, "Ergebnis empfangen: %s", payload);
                handle_result_message(payload);
            } else if (strcmp(topic, TOPIC_INCOMING_APDU_CMD) == 0) {
                handle_apdu_cmd_message(payload);
            } else if (strcmp(topic, TOPIC_INCOMING_HOMEKEY_GROUP_ID) == 0) {
                handle_homekey_group_id_message(payload);
            }
            free(payload);
            break;
        }

        default:
            break;
    }
}

esp_err_t mqtt_client_setup_init(const char *broker_uri, const char *username, const char *password)
{
    s_session_queue = xQueueCreate(8, sizeof(session_queue_item_t));

    // Default-Puffergroesse von esp-mqtt (1024 Byte) reicht seit
    // MQTT_APDU_MAX_LEN=2048 nicht mehr fuer ein hex-kodiertes apdu_cmd/
    // apdu_resp-JSON (~4100 Byte) -- sonst wuerde die Nachricht von esp-mqtt
    // intern schon vor MQTT_EVENT_DATA abgeschnitten/verworfen, unabhaengig
    // vom lokalen payload-Puffer oben.
    size_t mqtt_buffer_size = MQTT_APDU_MAX_LEN * 2 + 256;
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = username,
        .credentials.authentication.password = password,
        .buffer.size = mqtt_buffer_size,
        .buffer.out_size = mqtt_buffer_size,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_mqtt_client);
}

void mqtt_client_setup_publish_card(const uint8_t *uid, uint8_t uid_len, uint8_t sak,
                                     const uint8_t atqa[2], uint32_t session_id, bool iso14443_4)
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
    cJSON_AddNumberToObject(json, "session_id", session_id);
    cJSON_AddBoolToObject(json, "iso14443_4", iso14443_4);

    char *payload = cJSON_PrintUnformatted(json);
    ESP_LOGI(TAG, "Sende Kartendaten: %s", payload);

    esp_mqtt_client_publish(s_mqtt_client, TOPIC_OUTGOING_RAW, payload, 0, 1, 0);

    free(payload);
    cJSON_Delete(json);
}

void mqtt_client_setup_publish_apdu_response(uint32_t session_id, bool ok,
                                              const uint8_t *resp, size_t resp_len,
                                              const char *error)
{
    if (s_mqtt_client == NULL) return;

    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "session_id", session_id);
    cJSON_AddBoolToObject(json, "ok", ok);

    if (ok && resp != NULL) {
        char *resp_hex = malloc(resp_len * 2 + 1);
        if (resp_hex != NULL) {
            for (size_t i = 0; i < resp_len; i++) {
                snprintf(&resp_hex[i * 2], 3, "%02X", resp[i]);
            }
            cJSON_AddStringToObject(json, "response_hex", resp_hex);
            free(resp_hex);
        }
    }
    if (!ok && error != NULL) {
        cJSON_AddStringToObject(json, "error", error);
    }

    char *payload = cJSON_PrintUnformatted(json);
    esp_mqtt_client_publish(s_mqtt_client, TOPIC_OUTGOING_APDU_RESP, payload, 0, 0, 0);  // QoS 0, siehe Kommentar bei der Subscription
    free(payload);
    cJSON_Delete(json);
}

bool mqtt_client_setup_wait_apdu_cmd(uint32_t session_id, mqtt_apdu_cmd_t *out_cmd,
                                      bool *out_session_ended, uint32_t timeout_ms)
{
    *out_session_ended = false;
    if (s_session_queue == NULL) return false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return false;

        session_queue_item_t item;
        if (xQueueReceive(s_session_queue, &item, deadline - now) != pdTRUE) {
            return false;  // Timeout
        }

        if (item.session_id != session_id && item.session_id != 0) {
            continue;  // Nachricht einer alten/anderen Session, verwerfen
        }

        if (item.is_session_end) {
            *out_session_ended = true;
            return false;
        }

        out_cmd->session_id = item.session_id;
        out_cmd->apdu_len = item.apdu_len;
        memcpy(out_cmd->apdu, item.apdu, item.apdu_len);
        return true;
    }
}
