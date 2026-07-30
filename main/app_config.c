#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_config";
static const char *NVS_NAMESPACE = "cfg";

// Bisherige Compile-Time-Defaults (siehe alte MQTT_BROKER_URI/-Topics in
// main.c/mqtt_client_setup.c) -- damit ein frisch geflashtes Geraet ohne
// gespeicherte Config unveraendert weiterlaeuft.
#define DEFAULT_MQTT_BROKER_URI  "mqtt://10.60.16.71:1883"
#define DEFAULT_MQTT_USERNAME    "mqtt"
#define DEFAULT_MQTT_PASSWORD    "mqtt-2025!"
#define DEFAULT_TOPIC_RAW               "nfc/raw"
#define DEFAULT_TOPIC_APDU_CMD          "nfc/apdu_cmd"
#define DEFAULT_TOPIC_APDU_RESP         "nfc/apdu_resp"
#define DEFAULT_TOPIC_RESULT             "nfc/result"
#define DEFAULT_TOPIC_HOMEKEY_GROUP_ID   "nfc/homekey_group_id"
#define DEFAULT_RELAY_PULSE_MS   1500
#define DEFAULT_TOPIC_RELAY_PULSE_MS     "nfc/relay_pulse_ms"
#define DEFAULT_HOSTNAME         "wt32-nfc-gateway"
#define DEFAULT_ADMIN_PASSWORD   "admin"

static void get_str(nvs_handle_t h, const char *key, char *out, size_t out_cap, const char *dflt)
{
    size_t len = out_cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strncpy(out, dflt, out_cap - 1);
        out[out_cap - 1] = '\0';
    }
}

esp_err_t app_config_load(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // Namespace existiert noch nicht (erster Boot) -- komplett auf
        // Defaults zurueckfallen.
        ESP_LOGI(TAG, "Keine gespeicherte Konfiguration gefunden, verwende Defaults");
        cfg->net_use_dhcp = true;
        strncpy(cfg->hostname, DEFAULT_HOSTNAME, sizeof(cfg->hostname) - 1);
        strncpy(cfg->mqtt_broker_uri, DEFAULT_MQTT_BROKER_URI, sizeof(cfg->mqtt_broker_uri) - 1);
        strncpy(cfg->mqtt_username, DEFAULT_MQTT_USERNAME, sizeof(cfg->mqtt_username) - 1);
        strncpy(cfg->mqtt_password, DEFAULT_MQTT_PASSWORD, sizeof(cfg->mqtt_password) - 1);
        strncpy(cfg->topic_raw, DEFAULT_TOPIC_RAW, sizeof(cfg->topic_raw) - 1);
        strncpy(cfg->topic_apdu_cmd, DEFAULT_TOPIC_APDU_CMD, sizeof(cfg->topic_apdu_cmd) - 1);
        strncpy(cfg->topic_apdu_resp, DEFAULT_TOPIC_APDU_RESP, sizeof(cfg->topic_apdu_resp) - 1);
        strncpy(cfg->topic_result, DEFAULT_TOPIC_RESULT, sizeof(cfg->topic_result) - 1);
        strncpy(cfg->topic_homekey_group_id, DEFAULT_TOPIC_HOMEKEY_GROUP_ID, sizeof(cfg->topic_homekey_group_id) - 1);
        cfg->relay_pulse_ms = DEFAULT_RELAY_PULSE_MS;
        cfg->relay_pulse_via_mqtt = false;
        strncpy(cfg->topic_relay_pulse_ms, DEFAULT_TOPIC_RELAY_PULSE_MS, sizeof(cfg->topic_relay_pulse_ms) - 1);
        strncpy(cfg->admin_password, DEFAULT_ADMIN_PASSWORD, sizeof(cfg->admin_password) - 1);
        return ESP_OK;
    }

    uint8_t dhcp_u8 = 1;
    nvs_get_u8(h, "net_dhcp", &dhcp_u8);
    cfg->net_use_dhcp = dhcp_u8 != 0;

    get_str(h, "net_ip", cfg->net_ip, sizeof(cfg->net_ip), "");
    get_str(h, "net_gw", cfg->net_gateway, sizeof(cfg->net_gateway), "");
    get_str(h, "net_mask", cfg->net_netmask, sizeof(cfg->net_netmask), "255.255.255.0");
    get_str(h, "net_dns", cfg->net_dns, sizeof(cfg->net_dns), "");
    get_str(h, "hostname", cfg->hostname, sizeof(cfg->hostname), DEFAULT_HOSTNAME);

    get_str(h, "mqtt_uri", cfg->mqtt_broker_uri, sizeof(cfg->mqtt_broker_uri), DEFAULT_MQTT_BROKER_URI);
    get_str(h, "mqtt_user", cfg->mqtt_username, sizeof(cfg->mqtt_username), DEFAULT_MQTT_USERNAME);
    get_str(h, "mqtt_pass", cfg->mqtt_password, sizeof(cfg->mqtt_password), DEFAULT_MQTT_PASSWORD);
    get_str(h, "mqtt_cid", cfg->mqtt_client_id, sizeof(cfg->mqtt_client_id), "");

    get_str(h, "t_raw", cfg->topic_raw, sizeof(cfg->topic_raw), DEFAULT_TOPIC_RAW);
    get_str(h, "t_apdu_cmd", cfg->topic_apdu_cmd, sizeof(cfg->topic_apdu_cmd), DEFAULT_TOPIC_APDU_CMD);
    get_str(h, "t_apdu_resp", cfg->topic_apdu_resp, sizeof(cfg->topic_apdu_resp), DEFAULT_TOPIC_APDU_RESP);
    get_str(h, "t_result", cfg->topic_result, sizeof(cfg->topic_result), DEFAULT_TOPIC_RESULT);
    get_str(h, "t_homekey", cfg->topic_homekey_group_id, sizeof(cfg->topic_homekey_group_id), DEFAULT_TOPIC_HOMEKEY_GROUP_ID);

    uint32_t pulse = DEFAULT_RELAY_PULSE_MS;
    nvs_get_u32(h, "relay_ms", &pulse);
    cfg->relay_pulse_ms = pulse;

    uint8_t relay_mqtt_u8 = 0;
    nvs_get_u8(h, "relay_mqtt", &relay_mqtt_u8);
    cfg->relay_pulse_via_mqtt = relay_mqtt_u8 != 0;
    get_str(h, "t_relay_ms", cfg->topic_relay_pulse_ms, sizeof(cfg->topic_relay_pulse_ms), DEFAULT_TOPIC_RELAY_PULSE_MS);

    get_str(h, "admin_pass", cfg->admin_password, sizeof(cfg->admin_password), DEFAULT_ADMIN_PASSWORD);

    nvs_close(h);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (RW) fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h, "net_dhcp", cfg->net_use_dhcp ? 1 : 0);
    nvs_set_str(h, "net_ip", cfg->net_ip);
    nvs_set_str(h, "net_gw", cfg->net_gateway);
    nvs_set_str(h, "net_mask", cfg->net_netmask);
    nvs_set_str(h, "net_dns", cfg->net_dns);
    nvs_set_str(h, "hostname", cfg->hostname);

    nvs_set_str(h, "mqtt_uri", cfg->mqtt_broker_uri);
    nvs_set_str(h, "mqtt_user", cfg->mqtt_username);
    nvs_set_str(h, "mqtt_pass", cfg->mqtt_password);
    nvs_set_str(h, "mqtt_cid", cfg->mqtt_client_id);

    nvs_set_str(h, "t_raw", cfg->topic_raw);
    nvs_set_str(h, "t_apdu_cmd", cfg->topic_apdu_cmd);
    nvs_set_str(h, "t_apdu_resp", cfg->topic_apdu_resp);
    nvs_set_str(h, "t_result", cfg->topic_result);
    nvs_set_str(h, "t_homekey", cfg->topic_homekey_group_id);

    nvs_set_u32(h, "relay_ms", cfg->relay_pulse_ms);
    nvs_set_u8(h, "relay_mqtt", cfg->relay_pulse_via_mqtt ? 1 : 0);
    nvs_set_str(h, "t_relay_ms", cfg->topic_relay_pulse_ms);

    nvs_set_str(h, "admin_pass", cfg->admin_password);

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fehlgeschlagen: %s", esp_err_to_name(err));
    }
    return err;
}
