#include "web_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "web_config";

#define FORM_BUF_SIZE 2048

// -------------------- HTTP Basic Auth --------------------

static bool check_basic_auth(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_load(&cfg);

    char auth_header[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    const char *prefix = "Basic ";
    size_t prefix_len = strlen(prefix);
    if (strncmp(auth_header, prefix, prefix_len) != 0) {
        return false;
    }

    unsigned char decoded[128] = {0};
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                     (const unsigned char *)auth_header + prefix_len,
                                     strlen(auth_header + prefix_len));
    if (ret != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    char expected[APP_CFG_STR_LEN + 8];
    snprintf(expected, sizeof(expected), "admin:%s", cfg.admin_password);

    return strcmp((const char *)decoded, expected) == 0;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    if (check_basic_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WT32-NFC-Gateway\"");
    httpd_resp_send(req, "Login erforderlich", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

// -------------------- Hilfsfunktionen --------------------

// Ersetzt &, <, >, " durch HTML-Entities -- Config-Werte werden weiter unten
// in Attribute/Textinhalte des Formulars eingebettet und koennten (z.B. nach
// einem kompromittierten Login) Sonderzeichen enthalten.
static void html_escape(const char *in, char *out, size_t out_cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 6 < out_cap; i++) {
        switch (in[i]) {
            case '&':  o += snprintf(out + o, out_cap - o, "&amp;"); break;
            case '<':  o += snprintf(out + o, out_cap - o, "&lt;"); break;
            case '>':  o += snprintf(out + o, out_cap - o, "&gt;"); break;
            case '"':  o += snprintf(out + o, out_cap - o, "&quot;"); break;
            default:   out[o++] = in[i]; break;
        }
    }
    out[o] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Dekodiert x-www-form-urlencoded in-place (Ergebnis ist immer <= Eingabelaenge).
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && hex_val(r[1]) >= 0 && hex_val(r[2]) >= 0) {
            *w++ = (char)((hex_val(r[1]) << 4) | hex_val(r[2]));
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// Sucht "key=value" im (bereits urldekodierten Kopien-unabhaengigen) Rohbody
// und kopiert den (noch kodierten) Wert nach out; dekodiert danach in-place.
static void form_get(const char *body, const char *key, char *out, size_t out_cap)
{
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = body;
    while (p != NULL) {
        if ((p == body || p[-1] == '&') && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val_start = p + key_len + 1;
            const char *val_end = strchr(val_start, '&');
            size_t val_len = val_end != NULL ? (size_t)(val_end - val_start) : strlen(val_start);
            if (val_len >= out_cap) val_len = out_cap - 1;
            memcpy(out, val_start, val_len);
            out[val_len] = '\0';
            url_decode(out);
            return;
        }
        p = strchr(p, '&');
        if (p != NULL) p++;
    }
}

static bool form_get_bool(const char *body, const char *key)
{
    char tmp[8];
    form_get(body, key, tmp, sizeof(tmp));
    return tmp[0] != '\0';
}

// -------------------- GET / : Formular anzeigen --------------------

static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    app_config_t cfg;
    app_config_load(&cfg);

    // Fuer jedes Textfeld eine HTML-escapte Kopie -- Puffer bewusst pro Feld,
    // damit ein einzelner ueberlanger Wert nicht die anderen verdraengt.
    char e_ip[APP_CFG_IP_LEN * 2], e_gw[APP_CFG_IP_LEN * 2], e_mask[APP_CFG_IP_LEN * 2], e_dns[APP_CFG_IP_LEN * 2];
    char e_hostname[APP_CFG_STR_LEN * 2];
    char e_uri[APP_CFG_STR_LEN * 2], e_user[APP_CFG_STR_LEN * 2], e_pass[APP_CFG_STR_LEN * 2], e_cid[APP_CFG_STR_LEN * 2];
    char e_t_raw[APP_CFG_STR_LEN * 2], e_t_cmd[APP_CFG_STR_LEN * 2], e_t_resp[APP_CFG_STR_LEN * 2];
    char e_t_result[APP_CFG_STR_LEN * 2], e_t_hk[APP_CFG_STR_LEN * 2];
    char e_t_relay_ms[APP_CFG_STR_LEN * 2];
    char e_admin_pass[APP_CFG_STR_LEN * 2];

    html_escape(cfg.net_ip, e_ip, sizeof(e_ip));
    html_escape(cfg.net_gateway, e_gw, sizeof(e_gw));
    html_escape(cfg.net_netmask, e_mask, sizeof(e_mask));
    html_escape(cfg.net_dns, e_dns, sizeof(e_dns));
    html_escape(cfg.hostname, e_hostname, sizeof(e_hostname));
    html_escape(cfg.mqtt_broker_uri, e_uri, sizeof(e_uri));
    html_escape(cfg.mqtt_username, e_user, sizeof(e_user));
    html_escape(cfg.mqtt_password, e_pass, sizeof(e_pass));
    html_escape(cfg.mqtt_client_id, e_cid, sizeof(e_cid));
    html_escape(cfg.topic_raw, e_t_raw, sizeof(e_t_raw));
    html_escape(cfg.topic_apdu_cmd, e_t_cmd, sizeof(e_t_cmd));
    html_escape(cfg.topic_apdu_resp, e_t_resp, sizeof(e_t_resp));
    html_escape(cfg.topic_result, e_t_result, sizeof(e_t_result));
    html_escape(cfg.topic_homekey_group_id, e_t_hk, sizeof(e_t_hk));
    html_escape(cfg.topic_relay_pulse_ms, e_t_relay_ms, sizeof(e_t_relay_ms));
    html_escape(cfg.admin_password, e_admin_pass, sizeof(e_admin_pass));

    // Ein einzelner malloc-Puffer fuer die zusammengesetzte Seite --
    // deutlich einfacher als httpd_resp_sendstr_chunk-Ketten, und der Server
    // laeuft ohnehin nur auf Menschen-Anfrage, nicht im NFC-Hotpath.
    size_t html_cap = 8192;
    char *html = malloc(html_cap);
    if (html == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    snprintf(html, html_cap,
        "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>WT32-ETH01 NFC-Gateway Konfiguration</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:640px;margin:1.5em auto;padding:0 1em;color:#222}"
        "fieldset{margin-bottom:1.2em;border:1px solid #ccc;border-radius:6px}"
        "legend{font-weight:bold;padding:0 .4em}"
        "label{display:block;margin-top:.6em;font-size:.9em;color:#444}"
        "input[type=text],input[type=password],input[type=number]{width:100%%;box-sizing:border-box;padding:.4em;margin-top:.2em}"
        "button{margin-top:1.2em;padding:.6em 1.4em;font-size:1em;cursor:pointer}"
        ".row{display:flex;gap:1em}.row > div{flex:1}"
        "</style></head><body>"
        "<h2>WT32-ETH01 NFC-Gateway</h2>"
        "<form method=\"POST\" action=\"/save\">"

        "<fieldset><legend>Netzwerk</legend>"
        "<label><input type=\"checkbox\" name=\"dhcp\" %s onclick=\"toggleStatic(this)\"> DHCP verwenden</label>"
        "<div id=\"staticFields\">"
        "<div class=\"row\"><div><label>IP-Adresse<input type=\"text\" name=\"ip\" value=\"%s\" placeholder=\"192.168.1.50\"></label></div>"
        "<div><label>Subnetzmaske<input type=\"text\" name=\"mask\" value=\"%s\" placeholder=\"255.255.255.0\"></label></div></div>"
        "<div class=\"row\"><div><label>Gateway<input type=\"text\" name=\"gw\" value=\"%s\" placeholder=\"192.168.1.1\"></label></div>"
        "<div><label>DNS-Server<input type=\"text\" name=\"dns\" value=\"%s\" placeholder=\"192.168.1.1\"></label></div></div>"
        "</div>"
        "<label>Hostname<input type=\"text\" name=\"hostname\" value=\"%s\"></label>"
        "</fieldset>",
        cfg.net_use_dhcp ? "checked" : "", e_ip, e_mask, e_gw, e_dns, e_hostname);

    size_t used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<fieldset><legend>MQTT</legend>"
        "<label>Broker-URI<input type=\"text\" name=\"mqtt_uri\" value=\"%s\" placeholder=\"mqtt://host:1883\"></label>"
        "<div class=\"row\"><div><label>Benutzername<input type=\"text\" name=\"mqtt_user\" value=\"%s\"></label></div>"
        "<div><label>Passwort<input type=\"text\" name=\"mqtt_pass\" value=\"%s\"></label></div></div>"
        "<label>Client-ID (leer = automatisch)<input type=\"text\" name=\"mqtt_cid\" value=\"%s\"></label>"
        "<label>Topic: Karten-Rohdaten (ESP32 -&gt; Addon)<input type=\"text\" name=\"t_raw\" value=\"%s\"></label>"
        "<label>Topic: APDU-Kommando (Addon -&gt; ESP32)<input type=\"text\" name=\"t_apdu_cmd\" value=\"%s\"></label>"
        "<label>Topic: APDU-Antwort (ESP32 -&gt; Addon)<input type=\"text\" name=\"t_apdu_resp\" value=\"%s\"></label>"
        "<label>Topic: Ergebnis (Addon -&gt; ESP32)<input type=\"text\" name=\"t_result\" value=\"%s\"></label>"
        "<label>Topic: HomeKey Reader-Group-ID (Addon -&gt; ESP32)<input type=\"text\" name=\"t_homekey\" value=\"%s\"></label>"
        "</fieldset>",
        e_uri, e_user, e_pass, e_cid, e_t_raw, e_t_cmd, e_t_resp, e_t_result, e_t_hk);

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<fieldset><legend>Relais</legend>"
        "<label><input type=\"checkbox\" name=\"relay_mqtt\" %s onclick=\"toggleRelaySource(this)\"> Pulsdauer per MQTT setzen (statt fest)</label>"
        "<div id=\"relayFixedField\">"
        "<label>Pulsdauer (ms)<input type=\"number\" name=\"relay_ms\" value=\"%" PRIu32 "\" min=\"50\" max=\"10000\"></label>"
        "</div>"
        "<div id=\"relayMqttField\">"
        "<label>Topic: Relais-Pulsdauer (Addon -&gt; ESP32, retained, Payload = ms als Zahl)"
        "<input type=\"text\" name=\"t_relay_ms\" value=\"%s\"></label>"
        "</div>"
        "</fieldset>"

        "<fieldset><legend>WebGUI-Login</legend>"
        "<label>Admin-Passwort<input type=\"text\" name=\"admin_pass\" value=\"%s\"></label>"
        "</fieldset>"

        "<button type=\"submit\">Speichern &amp; Neustarten</button>"
        "</form>"
        "<script>"
        "function toggleStatic(cb){document.getElementById('staticFields').style.display=cb.checked?'none':'block';}"
        "toggleStatic(document.querySelector('input[name=dhcp]'));"
        "function toggleRelaySource(cb){"
        "document.getElementById('relayFixedField').style.display=cb.checked?'none':'block';"
        "document.getElementById('relayMqttField').style.display=cb.checked?'block':'none';"
        "}"
        "toggleRelaySource(document.querySelector('input[name=relay_mqtt]'));"
        "</script>"
        "</body></html>",
        cfg.relay_pulse_via_mqtt ? "checked" : "", cfg.relay_pulse_ms, e_t_relay_ms, e_admin_pass);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ESP_OK;
}

// -------------------- POST /save : Formular verarbeiten --------------------

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    if (req->content_len >= FORM_BUF_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Formular zu gross");
        return ESP_FAIL;
    }

    char *body = malloc(FORM_BUF_SIZE);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, body + total, req->content_len - total);
        if (r <= 0) {
            free(body);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        total += r;
    }
    body[total] = '\0';

    app_config_t cfg;
    app_config_load(&cfg);  // vorhandene Werte als Basis, Formular ueberschreibt nur bekannte Felder

    cfg.net_use_dhcp = form_get_bool(body, "dhcp");
    form_get(body, "ip", cfg.net_ip, sizeof(cfg.net_ip));
    form_get(body, "mask", cfg.net_netmask, sizeof(cfg.net_netmask));
    form_get(body, "gw", cfg.net_gateway, sizeof(cfg.net_gateway));
    form_get(body, "dns", cfg.net_dns, sizeof(cfg.net_dns));
    form_get(body, "hostname", cfg.hostname, sizeof(cfg.hostname));

    form_get(body, "mqtt_uri", cfg.mqtt_broker_uri, sizeof(cfg.mqtt_broker_uri));
    form_get(body, "mqtt_user", cfg.mqtt_username, sizeof(cfg.mqtt_username));
    form_get(body, "mqtt_pass", cfg.mqtt_password, sizeof(cfg.mqtt_password));
    form_get(body, "mqtt_cid", cfg.mqtt_client_id, sizeof(cfg.mqtt_client_id));
    form_get(body, "t_raw", cfg.topic_raw, sizeof(cfg.topic_raw));
    form_get(body, "t_apdu_cmd", cfg.topic_apdu_cmd, sizeof(cfg.topic_apdu_cmd));
    form_get(body, "t_apdu_resp", cfg.topic_apdu_resp, sizeof(cfg.topic_apdu_resp));
    form_get(body, "t_result", cfg.topic_result, sizeof(cfg.topic_result));
    form_get(body, "t_homekey", cfg.topic_homekey_group_id, sizeof(cfg.topic_homekey_group_id));

    cfg.relay_pulse_via_mqtt = form_get_bool(body, "relay_mqtt");
    form_get(body, "t_relay_ms", cfg.topic_relay_pulse_ms, sizeof(cfg.topic_relay_pulse_ms));
    if (cfg.topic_relay_pulse_ms[0] == '\0') {
        strncpy(cfg.topic_relay_pulse_ms, "nfc/relay_pulse_ms", sizeof(cfg.topic_relay_pulse_ms) - 1);
    }

    char relay_ms_str[16];
    form_get(body, "relay_ms", relay_ms_str, sizeof(relay_ms_str));
    if (relay_ms_str[0] != '\0') {
        long v = strtol(relay_ms_str, NULL, 10);
        if (v >= 50 && v <= 10000) {
            cfg.relay_pulse_ms = (uint32_t)v;
        }
    }

    char admin_pass[APP_CFG_STR_LEN];
    form_get(body, "admin_pass", admin_pass, sizeof(admin_pass));
    if (admin_pass[0] != '\0') {
        strncpy(cfg.admin_password, admin_pass, sizeof(cfg.admin_password) - 1);
    }

    free(body);

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Konfiguration gespeichert, starte in Kuerze neu...");

    const char *resp =
        "<!DOCTYPE html><html lang=\"de\"><head><meta charset=\"utf-8\">"
        "<title>Gespeichert</title></head><body>"
        "<p>Konfiguration gespeichert. Das Geraet startet jetzt neu...</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    // Kurze Verzoegerung, damit die Antwort den Client sicher erreicht, bevor
    // der Neustart die Verbindung kappt.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// -------------------- Server-Start --------------------

esp_err_t web_config_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Niedrigere Prioritaet und kleinerer Stack als card_event_task (Prio 5,
    // main.c) -- ein Seitenaufruf soll die NFC-Polling-/APDU-Relay-Task unter
    // Last niemals verdraengen, sondern selbst zurueckstehen.
    config.task_priority = tskIDLE_PRIORITY + 3;
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "Config-WebGUI gestartet auf Port %d", config.server_port);
    return ESP_OK;
}
