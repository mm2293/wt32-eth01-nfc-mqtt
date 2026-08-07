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
#define DEFAULT_TOPIC_RELAY_STATE        "nfc/relay_state"
#define DEFAULT_TOPIC_REED_STATE         "nfc/lock_reed_state"
#define DEFAULT_LOCK_SETTLE_DELAY_MS      5000
#define DEFAULT_TOPIC_LOCK_SETTLE_DELAY_MS "nfc/lock_settle_delay_ms"
#define DEFAULT_QOS_LOCK_SETTLE_MS        1
#define DEFAULT_HOSTNAME         "wt32-nfc-gateway"
#define DEFAULT_ADMIN_PASSWORD   "admin"
#define DEFAULT_PN532_RAW_BRIDGE_MODE  false
#define DEFAULT_PN532_BRIDGE_TCP_PORT  4444
#define DEFAULT_MQTT_CLEAN_SESSION  true

// QoS-Defaults entsprechen dem bisherigen fest kodierten Verhalten (siehe
// Kommentare in mqtt_client_setup.c zu den jeweiligen Subscribe-/Publish-Aufrufen).
#define DEFAULT_QOS_RAW                   1
#define DEFAULT_QOS_APDU_CMD              0
#define DEFAULT_QOS_APDU_RESP             0
#define DEFAULT_QOS_RESULT                1
#define DEFAULT_QOS_HOMEKEY_GROUP_ID      1
#define DEFAULT_QOS_RELAY_PULSE_MS        1
#define DEFAULT_QOS_APDU_RELAY_TIMEOUT_MS 1
#define DEFAULT_QOS_REED_STATE            1
#define DEFAULT_QOS_RELAY_STATE           1
#define DEFAULT_RETAIN_RAW                false
#define DEFAULT_RETAIN_APDU_RESP          false
#define DEFAULT_RETAIN_REED_STATE         true
#define DEFAULT_RETAIN_RELAY_STATE        true

// Default-GPIO-Zuordnung entspricht dem bisherigen fest kodierten Verhalten
// (relay_control.c IO4, reed_contact.c IO2, pn532_uart.c TX=IO14/RX=IO15).
// IO12 fuer den neuen Schalterkontakt ist bislang unbenutzt.
#define DEFAULT_GPIO_RELAY      4
#define DEFAULT_GPIO_REED       2
#define DEFAULT_GPIO_SWITCH     12
#define DEFAULT_GPIO_PN532_TX   14
#define DEFAULT_GPIO_PN532_RX   15

const uint8_t APP_CFG_GPIO_POOL[APP_CFG_GPIO_POOL_LEN] = {39, 36, 15, 14, 12, 5, 4, 2};

bool app_config_gpio_in_pool(uint8_t pin)
{
    for (int i = 0; i < APP_CFG_GPIO_POOL_LEN; i++) {
        if (APP_CFG_GPIO_POOL[i] == pin) return true;
    }
    return false;
}

bool app_config_gpio_supports_output(uint8_t pin)
{
    // IO39/IO36 sind am ESP32 Input-only (keine Ausgangstreiber vorhanden).
    return app_config_gpio_in_pool(pin) && pin != 39 && pin != 36;
}

// Liest eine GPIO-Pin-Zuordnung; fehlende/ausserhalb des Pools liegende
// Werte (und, falls require_output, Input-only-Pins IO39/IO36) fallen auf
// dflt zurueck -- Duplikate ueber mehrere Funktionen hinweg werden hier NICHT
// erkannt (das passiert beim Speichern in web_config.c), ein manuell
// korrumpierter NVS-Eintrag fuehrt also bestenfalls zu falscher, nicht zu
// undefinierter Pin-Nutzung.
static uint8_t get_gpio(nvs_handle_t h, const char *key, uint8_t dflt, bool require_output)
{
    uint8_t v = dflt;
    if (nvs_get_u8(h, key, &v) != ESP_OK) {
        return dflt;
    }
    if (!app_config_gpio_in_pool(v)) {
        return dflt;
    }
    if (require_output && !app_config_gpio_supports_output(v)) {
        return dflt;
    }
    return v;
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t out_cap, const char *dflt)
{
    size_t len = out_cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strncpy(out, dflt, out_cap - 1);
        out[out_cap - 1] = '\0';
    }
}

// Liest ein QoS-Level (0-2); ungueltige/fehlende Werte fallen auf dflt
// zurueck -- ein per WebGUI-Formular manipulierter Wert ausserhalb 0-2
// koennte sonst als rohe Zahl an esp_mqtt_client_subscribe()/_publish()
// durchgereicht werden.
static uint8_t get_qos(nvs_handle_t h, const char *key, uint8_t dflt)
{
    uint8_t v = dflt;
    if (nvs_get_u8(h, key, &v) != ESP_OK || v > 2) {
        return dflt;
    }
    return v;
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
        cfg->mqtt_clean_session = DEFAULT_MQTT_CLEAN_SESSION;
        cfg->qos_raw = DEFAULT_QOS_RAW;
        cfg->qos_apdu_cmd = DEFAULT_QOS_APDU_CMD;
        cfg->qos_apdu_resp = DEFAULT_QOS_APDU_RESP;
        cfg->qos_result = DEFAULT_QOS_RESULT;
        cfg->qos_homekey_group_id = DEFAULT_QOS_HOMEKEY_GROUP_ID;
        cfg->qos_relay_pulse_ms = DEFAULT_QOS_RELAY_PULSE_MS;
        cfg->qos_apdu_relay_timeout_ms = DEFAULT_QOS_APDU_RELAY_TIMEOUT_MS;
        cfg->qos_reed_state = DEFAULT_QOS_REED_STATE;
        cfg->qos_relay_state = DEFAULT_QOS_RELAY_STATE;
        cfg->retain_raw = DEFAULT_RETAIN_RAW;
        cfg->retain_apdu_resp = DEFAULT_RETAIN_APDU_RESP;
        cfg->retain_reed_state = DEFAULT_RETAIN_REED_STATE;
        cfg->retain_relay_state = DEFAULT_RETAIN_RELAY_STATE;
        cfg->relay_pulse_ms = DEFAULT_RELAY_PULSE_MS;
        cfg->relay_pulse_via_mqtt = false;
        strncpy(cfg->topic_relay_pulse_ms, DEFAULT_TOPIC_RELAY_PULSE_MS, sizeof(cfg->topic_relay_pulse_ms) - 1);
        strncpy(cfg->topic_relay_state, DEFAULT_TOPIC_RELAY_STATE, sizeof(cfg->topic_relay_state) - 1);
        strncpy(cfg->topic_reed_state, DEFAULT_TOPIC_REED_STATE, sizeof(cfg->topic_reed_state) - 1);
        cfg->lock_settle_delay_ms = DEFAULT_LOCK_SETTLE_DELAY_MS;
        cfg->lock_settle_delay_via_mqtt = false;
        strncpy(cfg->topic_lock_settle_delay_ms, DEFAULT_TOPIC_LOCK_SETTLE_DELAY_MS, sizeof(cfg->topic_lock_settle_delay_ms) - 1);
        cfg->qos_lock_settle_ms = DEFAULT_QOS_LOCK_SETTLE_MS;
        strncpy(cfg->admin_password, DEFAULT_ADMIN_PASSWORD, sizeof(cfg->admin_password) - 1);
        cfg->pn532_raw_bridge_mode = DEFAULT_PN532_RAW_BRIDGE_MODE;
        cfg->pn532_bridge_tcp_port = DEFAULT_PN532_BRIDGE_TCP_PORT;
        cfg->gpio_relay = DEFAULT_GPIO_RELAY;
        cfg->gpio_reed = DEFAULT_GPIO_REED;
        cfg->gpio_switch = DEFAULT_GPIO_SWITCH;
        cfg->gpio_pn532_tx = DEFAULT_GPIO_PN532_TX;
        cfg->gpio_pn532_rx = DEFAULT_GPIO_PN532_RX;
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

    uint8_t clean_session_u8 = DEFAULT_MQTT_CLEAN_SESSION ? 1 : 0;
    nvs_get_u8(h, "mqtt_clean", &clean_session_u8);
    cfg->mqtt_clean_session = clean_session_u8 != 0;

    cfg->qos_raw = get_qos(h, "qos_raw", DEFAULT_QOS_RAW);
    cfg->qos_apdu_cmd = get_qos(h, "qos_cmd", DEFAULT_QOS_APDU_CMD);
    cfg->qos_apdu_resp = get_qos(h, "qos_resp", DEFAULT_QOS_APDU_RESP);
    cfg->qos_result = get_qos(h, "qos_result", DEFAULT_QOS_RESULT);
    cfg->qos_homekey_group_id = get_qos(h, "qos_hk", DEFAULT_QOS_HOMEKEY_GROUP_ID);
    cfg->qos_relay_pulse_ms = get_qos(h, "qos_relayms", DEFAULT_QOS_RELAY_PULSE_MS);
    cfg->qos_apdu_relay_timeout_ms = get_qos(h, "qos_timeout", DEFAULT_QOS_APDU_RELAY_TIMEOUT_MS);
    cfg->qos_reed_state = get_qos(h, "qos_reed", DEFAULT_QOS_REED_STATE);
    cfg->qos_relay_state = get_qos(h, "qos_relaystate", DEFAULT_QOS_RELAY_STATE);

    uint8_t retain_raw_u8 = DEFAULT_RETAIN_RAW ? 1 : 0;
    nvs_get_u8(h, "ret_raw", &retain_raw_u8);
    cfg->retain_raw = retain_raw_u8 != 0;

    uint8_t retain_resp_u8 = DEFAULT_RETAIN_APDU_RESP ? 1 : 0;
    nvs_get_u8(h, "ret_resp", &retain_resp_u8);
    cfg->retain_apdu_resp = retain_resp_u8 != 0;

    uint8_t retain_reed_u8 = DEFAULT_RETAIN_REED_STATE ? 1 : 0;
    nvs_get_u8(h, "ret_reed", &retain_reed_u8);
    cfg->retain_reed_state = retain_reed_u8 != 0;

    uint8_t retain_relaystate_u8 = DEFAULT_RETAIN_RELAY_STATE ? 1 : 0;
    nvs_get_u8(h, "ret_relaystate", &retain_relaystate_u8);
    cfg->retain_relay_state = retain_relaystate_u8 != 0;

    uint32_t pulse = DEFAULT_RELAY_PULSE_MS;
    nvs_get_u32(h, "relay_ms", &pulse);
    cfg->relay_pulse_ms = pulse;

    uint8_t relay_mqtt_u8 = 0;
    nvs_get_u8(h, "relay_mqtt", &relay_mqtt_u8);
    cfg->relay_pulse_via_mqtt = relay_mqtt_u8 != 0;
    get_str(h, "t_relay_ms", cfg->topic_relay_pulse_ms, sizeof(cfg->topic_relay_pulse_ms), DEFAULT_TOPIC_RELAY_PULSE_MS);

    get_str(h, "t_relaystate", cfg->topic_relay_state, sizeof(cfg->topic_relay_state), DEFAULT_TOPIC_RELAY_STATE);

    get_str(h, "t_reed", cfg->topic_reed_state, sizeof(cfg->topic_reed_state), DEFAULT_TOPIC_REED_STATE);

    uint32_t settle_delay = DEFAULT_LOCK_SETTLE_DELAY_MS;
    nvs_get_u32(h, "lock_settle", &settle_delay);
    cfg->lock_settle_delay_ms = settle_delay;

    uint8_t settle_mqtt_u8 = 0;
    nvs_get_u8(h, "lock_settle_mq", &settle_mqtt_u8);
    cfg->lock_settle_delay_via_mqtt = settle_mqtt_u8 != 0;
    get_str(h, "t_lock_settle", cfg->topic_lock_settle_delay_ms, sizeof(cfg->topic_lock_settle_delay_ms), DEFAULT_TOPIC_LOCK_SETTLE_DELAY_MS);
    cfg->qos_lock_settle_ms = get_qos(h, "qos_settle", DEFAULT_QOS_LOCK_SETTLE_MS);

    get_str(h, "admin_pass", cfg->admin_password, sizeof(cfg->admin_password), DEFAULT_ADMIN_PASSWORD);

    uint8_t raw_bridge_u8 = DEFAULT_PN532_RAW_BRIDGE_MODE ? 1 : 0;
    nvs_get_u8(h, "pn532_raw", &raw_bridge_u8);
    cfg->pn532_raw_bridge_mode = raw_bridge_u8 != 0;

    uint16_t bridge_port = DEFAULT_PN532_BRIDGE_TCP_PORT;
    nvs_get_u16(h, "pn532_port", &bridge_port);
    cfg->pn532_bridge_tcp_port = bridge_port;

    cfg->gpio_relay = get_gpio(h, "gpio_relay", DEFAULT_GPIO_RELAY, true);
    cfg->gpio_reed = get_gpio(h, "gpio_reed", DEFAULT_GPIO_REED, false);
    cfg->gpio_switch = get_gpio(h, "gpio_switch", DEFAULT_GPIO_SWITCH, false);
    cfg->gpio_pn532_tx = get_gpio(h, "gpio_pn532_tx", DEFAULT_GPIO_PN532_TX, true);
    cfg->gpio_pn532_rx = get_gpio(h, "gpio_pn532_rx", DEFAULT_GPIO_PN532_RX, false);

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

    nvs_set_u8(h, "mqtt_clean", cfg->mqtt_clean_session ? 1 : 0);

    nvs_set_u8(h, "qos_raw", cfg->qos_raw);
    nvs_set_u8(h, "qos_cmd", cfg->qos_apdu_cmd);
    nvs_set_u8(h, "qos_resp", cfg->qos_apdu_resp);
    nvs_set_u8(h, "qos_result", cfg->qos_result);
    nvs_set_u8(h, "qos_hk", cfg->qos_homekey_group_id);
    nvs_set_u8(h, "qos_relayms", cfg->qos_relay_pulse_ms);
    nvs_set_u8(h, "qos_timeout", cfg->qos_apdu_relay_timeout_ms);
    nvs_set_u8(h, "qos_reed", cfg->qos_reed_state);
    nvs_set_u8(h, "qos_relaystate", cfg->qos_relay_state);

    nvs_set_u8(h, "ret_raw", cfg->retain_raw ? 1 : 0);
    nvs_set_u8(h, "ret_resp", cfg->retain_apdu_resp ? 1 : 0);
    nvs_set_u8(h, "ret_reed", cfg->retain_reed_state ? 1 : 0);
    nvs_set_u8(h, "ret_relaystate", cfg->retain_relay_state ? 1 : 0);

    nvs_set_u32(h, "relay_ms", cfg->relay_pulse_ms);
    nvs_set_u8(h, "relay_mqtt", cfg->relay_pulse_via_mqtt ? 1 : 0);
    nvs_set_str(h, "t_relay_ms", cfg->topic_relay_pulse_ms);
    nvs_set_str(h, "t_relaystate", cfg->topic_relay_state);

    nvs_set_str(h, "t_reed", cfg->topic_reed_state);

    nvs_set_u32(h, "lock_settle", cfg->lock_settle_delay_ms);
    nvs_set_u8(h, "lock_settle_mq", cfg->lock_settle_delay_via_mqtt ? 1 : 0);
    nvs_set_str(h, "t_lock_settle", cfg->topic_lock_settle_delay_ms);
    nvs_set_u8(h, "qos_settle", cfg->qos_lock_settle_ms);

    nvs_set_str(h, "admin_pass", cfg->admin_password);

    nvs_set_u8(h, "pn532_raw", cfg->pn532_raw_bridge_mode ? 1 : 0);
    nvs_set_u16(h, "pn532_port", cfg->pn532_bridge_tcp_port);

    nvs_set_u8(h, "gpio_relay", cfg->gpio_relay);
    nvs_set_u8(h, "gpio_reed", cfg->gpio_reed);
    nvs_set_u8(h, "gpio_switch", cfg->gpio_switch);
    nvs_set_u8(h, "gpio_pn532_tx", cfg->gpio_pn532_tx);
    nvs_set_u8(h, "gpio_pn532_rx", cfg->gpio_pn532_rx);

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit fehlgeschlagen: %s", esp_err_to_name(err));
    }
    return err;
}
