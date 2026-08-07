#include "web_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "relay_control.h"
#include "lock_control.h"

static const char *TAG = "web_config";

// War 2048 -- reicht seit den QoS/Retain-Checkboxen und dem
// Reedkontakt/Schloss-Feldset (deutlich mehr Formularfelder) nicht mehr
// sicher fuer alle Felder auf Maximallaenge inkl. URL-Encoding.
#define FORM_BUF_SIZE 4096
#define OTA_RECV_BUF_SIZE 4096

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

// Liest ein QoS-Level (0-2) aus einem <select>-Feld; fehlende/ungueltige
// Werte fallen auf dflt zurueck (z.B. weil das Feld im Raw-Bridge-Modus per
// JS ausgeblendet war und der Browser es trotzdem mitsendet, oder ein
// manipulierter Request einen Wert ausserhalb 0-2 schickt).
static uint8_t form_get_qos(const char *body, const char *key, uint8_t dflt)
{
    char tmp[4];
    form_get(body, key, tmp, sizeof(tmp));
    if (tmp[0] == '\0') return dflt;
    long v = strtol(tmp, NULL, 10);
    if (v < 0 || v > 2) return dflt;
    return (uint8_t)v;
}

// Liest eine GPIO-Pin-Auswahl aus einem <select>-Feld; fehlende/ausserhalb
// des Pools liegende (und, falls require_output, Input-only-) Werte fallen
// auf dflt zurueck. Duplikate ueber mehrere Rollen hinweg werden hier NICHT
// erkannt -- das passiert erst nach dem Einlesen aller fuenf GPIO-Felder,
// siehe save_post_handler().
static uint8_t form_get_gpio(const char *body, const char *key, uint8_t dflt, bool require_output)
{
    char tmp[4];
    form_get(body, key, tmp, sizeof(tmp));
    if (tmp[0] == '\0') return dflt;
    long v = strtol(tmp, NULL, 10);
    if (v < 0 || v > 255) return dflt;
    uint8_t pin = (uint8_t)v;
    if (!app_config_gpio_in_pool(pin)) return dflt;
    if (require_output && !app_config_gpio_supports_output(pin)) return dflt;
    return pin;
}

// Liest ein Sekunden-Textfeld (ggf. mit Nachkommastellen) und liefert den
// Wert in Millisekunden, geklemmt auf [min_ms, max_ms]. Liefert -1 (statt
// eines uint32_t), wenn das Feld leer/ungueltig war, damit der Aufrufer den
// bisherigen Wert unangetastet lassen kann statt ihn auf 0 zu ueberschreiben.
static long form_get_seconds_as_ms(const char *body, const char *key, uint32_t min_ms, uint32_t max_ms)
{
    char tmp[24];
    form_get(body, key, tmp, sizeof(tmp));
    if (tmp[0] == '\0') return -1;

    char *endptr = NULL;
    double sec = strtod(tmp, &endptr);
    if (endptr == tmp || sec < 0) return -1;

    double ms = sec * 1000.0;
    if (ms < (double)min_ms) ms = (double)min_ms;
    if (ms > (double)max_ms) ms = (double)max_ms;
    return (long)(ms + 0.5);  // kaufmaennisch runden
}

// Baut ein <select> mit den drei moeglichen QoS-Stufen, aktueller Wert
// vorausgewaehlt -- val wird dabei auf 0-2 geklemmt (defensiv, falls je ein
// ungueltiger Wert aus NVS geladen wuerde).
static void render_qos_select(char *out, size_t out_cap, const char *name, uint8_t val)
{
    if (val > 2) val = 2;
    snprintf(out, out_cap,
        "<select name=\"%s\">"
        "<option value=\"0\"%s>QoS 0</option>"
        "<option value=\"1\"%s>QoS 1</option>"
        "<option value=\"2\"%s>QoS 2</option>"
        "</select>",
        name,
        val == 0 ? " selected" : "",
        val == 1 ? " selected" : "",
        val == 2 ? " selected" : "");
}

// Baut ein <select> mit allen Pins aus APP_CFG_GPIO_POOL, aktueller Wert
// vorausgewaehlt -- bei output_capable=true werden Input-only-Pins (IO39/
// IO36) ausgelassen, da sie fuer diese Rolle (Relais, PN532-TX) nicht
// waehlbar sind.
static void render_gpio_select(char *out, size_t out_cap, const char *name, uint8_t val, bool output_capable)
{
    size_t pos = 0;
    pos += snprintf(out + pos, out_cap - pos, "<select name=\"%s\">", name);
    for (int i = 0; i < APP_CFG_GPIO_POOL_LEN; i++) {
        uint8_t pin = APP_CFG_GPIO_POOL[i];
        if (output_capable && !app_config_gpio_supports_output(pin)) continue;
        pos += snprintf(out + pos, out_cap - pos, "<option value=\"%u\"%s>IO%u</option>",
                         pin, pin == val ? " selected" : "", pin);
    }
    snprintf(out + pos, out_cap - pos, "</select>");
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
    char e_t_relay_ms[APP_CFG_STR_LEN * 2], e_t_relaystate[APP_CFG_STR_LEN * 2];
    char e_t_reed[APP_CFG_STR_LEN * 2], e_t_lock_settle[APP_CFG_STR_LEN * 2];
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
    html_escape(cfg.topic_relay_state, e_t_relaystate, sizeof(e_t_relaystate));
    html_escape(cfg.topic_reed_state, e_t_reed, sizeof(e_t_reed));
    html_escape(cfg.topic_lock_settle_delay_ms, e_t_lock_settle, sizeof(e_t_lock_settle));
    html_escape(cfg.admin_password, e_admin_pass, sizeof(e_admin_pass));

    // Sekunden-Anzeige (Textfelder erwarten Sekunden, nicht mehr ms -- die
    // Config selbst bleibt intern in ms, siehe app_config.h). "%.3f" erlaubt
    // bei Bedarf weiterhin ms-genaue Werte (z.B. 1.5s).
    char sec_relay[24], sec_settle[24];
    snprintf(sec_relay, sizeof(sec_relay), "%.3f", cfg.relay_pulse_ms / 1000.0);
    snprintf(sec_settle, sizeof(sec_settle), "%.3f", cfg.lock_settle_delay_ms / 1000.0);

    // QoS-Dropdowns je Topic -- ein <select> pro konfigurierbarem Topic,
    // aktueller Wert vorausgewaehlt.
    char sel_qos_raw[200], sel_qos_cmd[200], sel_qos_resp[200], sel_qos_result[200];
    char sel_qos_hk[200], sel_qos_relayms[200], sel_qos_timeout[200];
    char sel_qos_reed[200], sel_qos_settle[200], sel_qos_relaystate[200];
    render_qos_select(sel_qos_raw, sizeof(sel_qos_raw), "qos_raw", cfg.qos_raw);
    render_qos_select(sel_qos_cmd, sizeof(sel_qos_cmd), "qos_cmd", cfg.qos_apdu_cmd);
    render_qos_select(sel_qos_resp, sizeof(sel_qos_resp), "qos_resp", cfg.qos_apdu_resp);
    render_qos_select(sel_qos_result, sizeof(sel_qos_result), "qos_result", cfg.qos_result);
    render_qos_select(sel_qos_hk, sizeof(sel_qos_hk), "qos_hk", cfg.qos_homekey_group_id);
    render_qos_select(sel_qos_relayms, sizeof(sel_qos_relayms), "qos_relayms", cfg.qos_relay_pulse_ms);
    render_qos_select(sel_qos_timeout, sizeof(sel_qos_timeout), "qos_timeout", cfg.qos_apdu_relay_timeout_ms);
    render_qos_select(sel_qos_reed, sizeof(sel_qos_reed), "qos_reed", cfg.qos_reed_state);
    render_qos_select(sel_qos_settle, sizeof(sel_qos_settle), "qos_settle", cfg.qos_lock_settle_ms);
    render_qos_select(sel_qos_relaystate, sizeof(sel_qos_relaystate), "qos_relaystate", cfg.qos_relay_state);

    // GPIO-Dropdowns fuer die frei zuweisbaren Funktionen.
    char sel_gpio_relay[400], sel_gpio_reed[400], sel_gpio_switch[400];
    char sel_gpio_pn532_tx[400], sel_gpio_pn532_rx[400];
    render_gpio_select(sel_gpio_relay, sizeof(sel_gpio_relay), "gpio_relay", cfg.gpio_relay, true);
    render_gpio_select(sel_gpio_reed, sizeof(sel_gpio_reed), "gpio_reed", cfg.gpio_reed, false);
    render_gpio_select(sel_gpio_switch, sizeof(sel_gpio_switch), "gpio_switch", cfg.gpio_switch, false);
    render_gpio_select(sel_gpio_pn532_tx, sizeof(sel_gpio_pn532_tx), "gpio_pn532_tx", cfg.gpio_pn532_tx, true);
    render_gpio_select(sel_gpio_pn532_rx, sizeof(sel_gpio_pn532_rx), "gpio_pn532_rx", cfg.gpio_pn532_rx, false);

    // Diagnose-Info fuer das OTA-Fieldset: aktuell laufende Partition und ob
    // ueberhaupt eine zweite OTA-Partition (siehe partitions.csv) vorhanden
    // ist. Ohne Custom-Partitionstabelle liefert esp_ota_get_next_update_partition()
    // NULL -- der Upload wuerde dann serverseitig mit klarer Fehlermeldung
    // abgelehnt, das wird hier schon vorab sichtbar gemacht.
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next_update = esp_ota_get_next_update_partition(NULL);
    char ota_info[192];
    if (next_update != NULL) {
        snprintf(ota_info, sizeof(ota_info),
                 "Laeuft aktuell von Partition <code>%s</code>. Naechstes Update geht nach <code>%s</code>.",
                 running != NULL ? running->label : "?", next_update->label);
    } else {
        snprintf(ota_info, sizeof(ota_info),
                 "Keine zweite OTA-Partition gefunden -- Custom-Partitionstabelle "
                 "(partitions.csv, siehe README) noetig, danach einmalig neu flashen.");
    }

    // Ein einzelner malloc-Puffer fuer die zusammengesetzte Seite --
    // deutlich einfacher als httpd_resp_sendstr_chunk-Ketten, und der Server
    // laeuft ohnehin nur auf Menschen-Anfrage, nicht im NFC-Hotpath.
    // War 11776 -- reicht seit den QoS/Retain-Dropdowns je Topic, dem
    // Reedkontakt/Schloss-Feldset und der GPIO-Zuordnung nicht mehr.
    size_t html_cap = 28672;
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
        "select{padding:.4em;margin-top:.2em}"
        "button{margin-top:1.2em;padding:.6em 1.4em;font-size:1em;cursor:pointer}"
        ".row{display:flex;gap:1em}.row > div{flex:1}"
        ".qosrow{display:flex;align-items:center;gap:.6em;margin-top:.3em}"
        ".qosrow label{margin-top:0;flex:0 0 auto}"
        ".inline{display:inline-flex;align-items:center;gap:.3em;font-size:.85em;color:#444}"
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
        "<fieldset><legend>PN532-Modus</legend>"
        "<label><input type=\"checkbox\" name=\"pn532_raw\" %s onclick=\"toggleRawBridgeFields(this)\"> Raw-Bridge statt Managed</label>"
        "<p style=\"font-size:.85em;color:#555\">Managed (Standard, unmarkiert): die Firmware pollt selbst nach "
        "Karten und relayt HomeKey/DESFire-APDUs per MQTT -- Zutrittssteuerung/Lernmodus funktionieren normal. "
        "Raw-Bridge (markiert): kein Kartenpolling mehr, die PN532-UART wird stattdessen 1:1 als TCP-Server "
        "exponiert, fuer direkten Zugriff durch das Addon/externe Tools (mfoc, libnfc, ...). In diesem Modus "
        "ist die automatische Zutrittssteuerung ueber diesen Reader inaktiv (die weiter unten im MQTT-Feldset "
        "ausgeblendeten Felder sind dann ohne Funktion). Relaissteuerung per MQTT inkl. Reedkontakt-Logik bleibt "
        "in BEIDEN Modi unveraendert aktiv. Eine Aenderung wird erst nach dem Neustart wirksam.</p>"
        "<label>Bridge TCP-Port (nur im Raw-Bridge-Modus relevant)"
        "<input type=\"number\" name=\"pn532_port\" value=\"%u\" min=\"1\" max=\"65535\"></label>"
        "</fieldset>",
        cfg.pn532_raw_bridge_mode ? "checked" : "", (unsigned)cfg.pn532_bridge_tcp_port);

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<fieldset><legend>GPIO-Zuordnung</legend>"
        "<p style=\"font-size:.85em;color:#555\">Pin-Zuordnung fuer Relais, Reedkontakt, Schalter/Taster und "
        "die PN532-UART. IO39/IO36 sind nur als Eingang nutzbar (kein Ausgangstreiber), daher bei Relais und "
        "PN532-TX nicht waehlbar. Jeder Pin darf nur einer Funktion zugewiesen sein -- ein Speichern mit "
        "doppelt vergebenem Pin wird abgelehnt, die vorherige Zuordnung bleibt dann unveraendert bestehen. "
        "IO12 ist ein Boot-Strapping-Pin (Flash-Spannungsauswahl): nur mit internem Pull-Up verwenden "
        "(Standard dieser Firmware fuer Reedkontakt/Schalter), keinen externen Pull-Up hinzufuegen. Eine "
        "Aenderung wird erst nach dem Neustart wirksam.</p>"
        "<div class=\"row\"><div><label>Relais (Ausgang)%s</label></div>"
        "<div><label>Reedkontakt (Eingang)%s</label></div></div>"
        "<div class=\"row\"><div><label>Schalter/Taster (Eingang, loest wie ein NFC-Zutritt aus)%s</label></div>"
        "<div><label>PN532 TX (Ausgang, -&gt; PN532 RX)%s</label></div></div>"
        "<label>PN532 RX (Eingang, PN532 TX -&gt;)%s</label>"
        "</fieldset>",
        sel_gpio_relay, sel_gpio_reed, sel_gpio_switch, sel_gpio_pn532_tx, sel_gpio_pn532_rx);

    char ret_raw_html[96], ret_resp_html[96], ret_reed_html[96], ret_relaystate_html[96];
    snprintf(ret_raw_html, sizeof(ret_raw_html), "<span class=\"inline\"><input type=\"checkbox\" name=\"ret_raw\" %s> Retain</span>", cfg.retain_raw ? "checked" : "");
    snprintf(ret_resp_html, sizeof(ret_resp_html), "<span class=\"inline\"><input type=\"checkbox\" name=\"ret_resp\" %s> Retain</span>", cfg.retain_apdu_resp ? "checked" : "");
    snprintf(ret_reed_html, sizeof(ret_reed_html), "<span class=\"inline\"><input type=\"checkbox\" name=\"ret_reed\" %s> Retain</span>", cfg.retain_reed_state ? "checked" : "");
    snprintf(ret_relaystate_html, sizeof(ret_relaystate_html), "<span class=\"inline\"><input type=\"checkbox\" name=\"ret_relaystate\" %s> Retain</span>", cfg.retain_relay_state ? "checked" : "");

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<fieldset><legend>MQTT</legend>"
        "<label>Broker-URI<input type=\"text\" name=\"mqtt_uri\" value=\"%s\" placeholder=\"mqtt://host:1883\"></label>"
        "<div class=\"row\"><div><label>Benutzername<input type=\"text\" name=\"mqtt_user\" value=\"%s\"></label></div>"
        "<div><label>Passwort<input type=\"text\" name=\"mqtt_pass\" value=\"%s\"></label></div></div>"
        "<label>Client-ID (leer = automatisch)<input type=\"text\" name=\"mqtt_cid\" value=\"%s\"></label>"
        "<label><input type=\"checkbox\" name=\"mqtt_clean\" %s> Clean Session</label>"
        "<p style=\"font-size:.85em;color:#555\">Verbindungsweite Einstellung (MQTT kennt das nur pro Client, "
        "nicht pro Topic): markiert (Standard) = der Broker verwirft Subscriptions/wartende Nachrichten bei "
        "Trennung. Unmarkiert = \"Persistent Session\", der Broker haelt beides ueber Trennungen hinweg vor -- "
        "braucht dafuer eine feste Client-ID oben (sonst wirkungslos).</p>"
        "<div id=\"managedOnlyFields\">"
        "<label>Topic: Karten-Rohdaten (ESP32 -&gt; Addon)<input type=\"text\" name=\"t_raw\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label>%s</div>",
        e_uri, e_user, e_pass, e_cid, cfg.mqtt_clean_session ? "checked" : "", e_t_raw, sel_qos_raw, ret_raw_html);

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<label>Topic: APDU-Kommando (Addon -&gt; ESP32)<input type=\"text\" name=\"t_apdu_cmd\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label></div>"
        "<label>Topic: APDU-Antwort (ESP32 -&gt; Addon)<input type=\"text\" name=\"t_apdu_resp\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label>%s</div>",
        e_t_cmd, sel_qos_cmd, e_t_resp, sel_qos_resp, ret_resp_html);

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<label>Topic: HomeKey Reader-Group-ID (Addon -&gt; ESP32)<input type=\"text\" name=\"t_homekey\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label></div>"
        "<p style=\"font-size:.85em;color:#555\">Der APDU-Relay-Timeout (wie lange die Karte zwischen Kommandos "
        "im Feld gehalten wird) wird nicht mehr hier konfiguriert, sondern vollstaendig vom Addon per retained "
        "MQTT-Topic <code>nfc/apdu_relay_timeout_ms</code> gesteuert (Standard 3000ms).</p>"
        "<div class=\"qosrow\"><label>QoS fuer <code>nfc/apdu_relay_timeout_ms</code> %s</label></div>"
        "</div>"
        "<label>Topic: Ergebnis (Addon -&gt; ESP32)<input type=\"text\" name=\"t_result\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label></div>"
        "</fieldset>",
        e_t_hk, sel_qos_hk, sel_qos_timeout, e_t_result, sel_qos_result);

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<fieldset><legend>Relais</legend>"
        "<label><input type=\"checkbox\" name=\"relay_mqtt\" %s onclick=\"toggleRelaySource(this)\"> Pulsdauer per MQTT setzen (statt fest)</label>"
        "<div id=\"relayFixedField\">"
        "<label>Basis-Pulsdauer (Sekunden)<input type=\"number\" name=\"relay_sec\" value=\"%s\" min=\"0.05\" step=\"0.05\"></label>"
        "</div>"
        "<div id=\"relayMqttField\">"
        "<label>Topic: Relais-Pulsdauer (Addon -&gt; ESP32, retained, Payload = ms als Zahl)"
        "<input type=\"text\" name=\"t_relay_ms\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label></div>"
        "</div>"
        "<p style=\"font-size:.85em;color:#555\">Kein festes Maximum mehr -- siehe Reedkontakt-Feldset unten: "
        "das Relais wird bei einem Zutrittsvorgang ohnehin so lange gehalten, wie es das Schloss laut "
        "Reedkontakt braucht, unabhaengig von dieser Basis-Pulsdauer.</p>"
        "<label>Topic: Relais-Status (ESP32 -&gt; Addon, retained, Payload \"on\"/\"off\")"
        "<input type=\"text\" name=\"t_relaystate\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label>%s</div>"
        "</fieldset>",
        cfg.relay_pulse_via_mqtt ? "checked" : "", sec_relay, e_t_relay_ms, sel_qos_relayms,
        e_t_relaystate, sel_qos_relaystate, ret_relaystate_html);

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<fieldset><legend>Reedkontakt &amp; Schloss-Logik</legend>"
        "<p style=\"font-size:.85em;color:#555\">Reedkontakt an IO2 (fest verdrahtet, Input mit internem "
        "Pull-Up gegen GND) meldet, ob das Schloss in Schliessposition ist. Nach einem gewaehrten Zutritt "
        "(<code>nfc/result</code> mit <code>granted:true</code>) haelt die Firmware das Relais so lange aktiv, "
        "wie der Reedkontakt \"nicht geschlossen\" meldet (z.B. weil die Tuer noch offen steht), plus der "
        "folgenden Nachlaufzeit nach dem Wiederschliessen. Siehe PROTOCOL.md fuer Details.</p>"
        "<label>Topic: Reedkontakt-Status (ESP32 -&gt; Addon, retained)<input type=\"text\" name=\"t_reed\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label>%s</div>"
        "<label><input type=\"checkbox\" name=\"lock_settle_mqtt\" %s onclick=\"toggleLockSettleSource(this)\"> Nachlaufzeit per MQTT setzen (statt fest)</label>"
        "<div id=\"lockSettleFixedField\">"
        "<label>Nachlaufzeit nach dem Wiederschliessen (Sekunden)<input type=\"number\" name=\"lock_settle_sec\" value=\"%s\" min=\"0\" step=\"0.1\"></label>"
        "</div>"
        "<div id=\"lockSettleMqttField\">"
        "<label>Topic: Schloss-Nachlaufzeit (Addon -&gt; ESP32, retained, Payload = ms als Zahl)"
        "<input type=\"text\" name=\"t_lock_settle\" value=\"%s\"></label>"
        "<div class=\"qosrow\"><label>QoS %s</label></div>"
        "</div>"
        "<p style=\"font-size:.85em;color:#555\">Kein zeitliches Limit fuers Halten -- das Relais bleibt aktiv, "
        "bis der Reedkontakt wieder \"geschlossen\" meldet, egal wie lange die Tuer offen steht.</p>"
        "</fieldset>",
        e_t_reed, sel_qos_reed, ret_reed_html, cfg.lock_settle_delay_via_mqtt ? "checked" : "",
        sec_settle, e_t_lock_settle, sel_qos_settle);

    used = strlen(html);
    snprintf(html + used, html_cap - used,
        "<fieldset><legend>WebGUI-Login</legend>"
        "<label>Admin-Passwort<input type=\"text\" name=\"admin_pass\" value=\"%s\"></label>"
        "</fieldset>"

        "<button type=\"submit\">Speichern &amp; Neustarten</button>"
        "</form>"

        "<fieldset><legend>Firmware-Update (OTA)</legend>"
        "<p style=\"font-size:.85em;color:#555\">%s</p>"
        "<label>Firmware-Datei (.bin)<input type=\"file\" id=\"otaFile\" accept=\".bin\"></label>"
        "<button type=\"button\" onclick=\"uploadOta()\">Hochladen &amp; Neustarten</button>"
        "<p id=\"otaStatus\"></p>"
        "</fieldset>"

        "<script>"
        "function toggleStatic(cb){document.getElementById('staticFields').style.display=cb.checked?'none':'block';}"
        "toggleStatic(document.querySelector('input[name=dhcp]'));"
        "function toggleRelaySource(cb){"
        "document.getElementById('relayFixedField').style.display=cb.checked?'none':'block';"
        "document.getElementById('relayMqttField').style.display=cb.checked?'block':'none';"
        "}"
        "toggleRelaySource(document.querySelector('input[name=relay_mqtt]'));"
        "function toggleLockSettleSource(cb){"
        "document.getElementById('lockSettleFixedField').style.display=cb.checked?'none':'block';"
        "document.getElementById('lockSettleMqttField').style.display=cb.checked?'block':'none';"
        "}"
        "toggleLockSettleSource(document.querySelector('input[name=lock_settle_mqtt]'));"
        "function toggleRawBridgeFields(cb){"
        "document.getElementById('managedOnlyFields').style.display=cb.checked?'none':'block';"
        "}"
        "toggleRawBridgeFields(document.querySelector('input[name=pn532_raw]'));"
        "async function uploadOta(){"
        "var f=document.getElementById('otaFile').files[0];"
        "var s=document.getElementById('otaStatus');"
        "if(!f){s.textContent='Bitte zuerst eine .bin-Datei auswaehlen.';return;}"
        "if(!confirm('Firmware \\''+f.name+'\\' ('+f.size+' Byte) wirklich aufspielen? Das Geraet startet danach neu.'))return;"
        "s.textContent='Lade hoch...';"
        "try{"
        "var r=await fetch('/ota',{method:'POST',body:f});"
        "var t=await r.text();"
        "s.textContent=t;"
        "}catch(e){s.textContent='Upload fehlgeschlagen: '+e;}"
        "}"
        "</script>"
        "</body></html>",
        e_admin_pass,
        ota_info);

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
    cfg.mqtt_clean_session = form_get_bool(body, "mqtt_clean");
    form_get(body, "t_raw", cfg.topic_raw, sizeof(cfg.topic_raw));
    form_get(body, "t_apdu_cmd", cfg.topic_apdu_cmd, sizeof(cfg.topic_apdu_cmd));
    form_get(body, "t_apdu_resp", cfg.topic_apdu_resp, sizeof(cfg.topic_apdu_resp));
    form_get(body, "t_result", cfg.topic_result, sizeof(cfg.topic_result));
    form_get(body, "t_homekey", cfg.topic_homekey_group_id, sizeof(cfg.topic_homekey_group_id));

    cfg.qos_raw = form_get_qos(body, "qos_raw", cfg.qos_raw);
    cfg.qos_apdu_cmd = form_get_qos(body, "qos_cmd", cfg.qos_apdu_cmd);
    cfg.qos_apdu_resp = form_get_qos(body, "qos_resp", cfg.qos_apdu_resp);
    cfg.qos_result = form_get_qos(body, "qos_result", cfg.qos_result);
    cfg.qos_homekey_group_id = form_get_qos(body, "qos_hk", cfg.qos_homekey_group_id);
    cfg.qos_relay_pulse_ms = form_get_qos(body, "qos_relayms", cfg.qos_relay_pulse_ms);
    cfg.qos_apdu_relay_timeout_ms = form_get_qos(body, "qos_timeout", cfg.qos_apdu_relay_timeout_ms);
    cfg.qos_reed_state = form_get_qos(body, "qos_reed", cfg.qos_reed_state);
    cfg.qos_lock_settle_ms = form_get_qos(body, "qos_settle", cfg.qos_lock_settle_ms);
    cfg.qos_relay_state = form_get_qos(body, "qos_relaystate", cfg.qos_relay_state);

    cfg.retain_raw = form_get_bool(body, "ret_raw");
    cfg.retain_apdu_resp = form_get_bool(body, "ret_resp");
    cfg.retain_reed_state = form_get_bool(body, "ret_reed");
    cfg.retain_relay_state = form_get_bool(body, "ret_relaystate");

    form_get(body, "t_reed", cfg.topic_reed_state, sizeof(cfg.topic_reed_state));
    if (cfg.topic_reed_state[0] == '\0') {
        strncpy(cfg.topic_reed_state, "nfc/lock_reed_state", sizeof(cfg.topic_reed_state) - 1);
    }

    cfg.relay_pulse_via_mqtt = form_get_bool(body, "relay_mqtt");
    form_get(body, "t_relay_ms", cfg.topic_relay_pulse_ms, sizeof(cfg.topic_relay_pulse_ms));
    if (cfg.topic_relay_pulse_ms[0] == '\0') {
        strncpy(cfg.topic_relay_pulse_ms, "nfc/relay_pulse_ms", sizeof(cfg.topic_relay_pulse_ms) - 1);
    }

    long relay_ms = form_get_seconds_as_ms(body, "relay_sec", RELAY_PULSE_MS_MIN, RELAY_PULSE_MS_MAX);
    if (relay_ms >= 0) {
        cfg.relay_pulse_ms = (uint32_t)relay_ms;
    }

    form_get(body, "t_relaystate", cfg.topic_relay_state, sizeof(cfg.topic_relay_state));
    if (cfg.topic_relay_state[0] == '\0') {
        strncpy(cfg.topic_relay_state, "nfc/relay_state", sizeof(cfg.topic_relay_state) - 1);
    }

    cfg.lock_settle_delay_via_mqtt = form_get_bool(body, "lock_settle_mqtt");
    form_get(body, "t_lock_settle", cfg.topic_lock_settle_delay_ms, sizeof(cfg.topic_lock_settle_delay_ms));
    if (cfg.topic_lock_settle_delay_ms[0] == '\0') {
        strncpy(cfg.topic_lock_settle_delay_ms, "nfc/lock_settle_delay_ms", sizeof(cfg.topic_lock_settle_delay_ms) - 1);
    }

    long settle_ms = form_get_seconds_as_ms(body, "lock_settle_sec", LOCK_SETTLE_DELAY_MS_MIN, LOCK_SETTLE_DELAY_MS_MAX);
    if (settle_ms >= 0) {
        cfg.lock_settle_delay_ms = (uint32_t)settle_ms;
    }

    char admin_pass[APP_CFG_STR_LEN];
    form_get(body, "admin_pass", admin_pass, sizeof(admin_pass));
    if (admin_pass[0] != '\0') {
        strncpy(cfg.admin_password, admin_pass, sizeof(cfg.admin_password) - 1);
    }

    cfg.pn532_raw_bridge_mode = form_get_bool(body, "pn532_raw");
    char pn532_port_str[8];
    form_get(body, "pn532_port", pn532_port_str, sizeof(pn532_port_str));
    if (pn532_port_str[0] != '\0') {
        long v = strtol(pn532_port_str, NULL, 10);
        if (v >= 1 && v <= 65535) {
            cfg.pn532_bridge_tcp_port = (uint16_t)v;
        }
    }

    // GPIO-Zuordnung: erst alle fuenf neuen Werte einzeln gegen den Pool/
    // die Ausgangsfaehigkeit pruefen (form_get_gpio() faellt bei ungueltigen
    // Werten je einzeln auf den bisherigen Wert zurueck), dann als Ganzes
    // auf Duplikate zwischen den Rollen pruefen -- bei einem Konflikt bleibt
    // die GESAMTE bisherige Zuordnung unveraendert (kein Teil-Uebernehmen),
    // damit nie zwei Funktionen denselben Pin belegen.
    uint8_t new_gpio_relay = form_get_gpio(body, "gpio_relay", cfg.gpio_relay, true);
    uint8_t new_gpio_reed = form_get_gpio(body, "gpio_reed", cfg.gpio_reed, false);
    uint8_t new_gpio_switch = form_get_gpio(body, "gpio_switch", cfg.gpio_switch, false);
    uint8_t new_gpio_pn532_tx = form_get_gpio(body, "gpio_pn532_tx", cfg.gpio_pn532_tx, true);
    uint8_t new_gpio_pn532_rx = form_get_gpio(body, "gpio_pn532_rx", cfg.gpio_pn532_rx, false);

    uint8_t new_gpios[5] = {new_gpio_relay, new_gpio_reed, new_gpio_switch, new_gpio_pn532_tx, new_gpio_pn532_rx};
    bool gpio_conflict = false;
    for (int i = 0; i < 5 && !gpio_conflict; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (new_gpios[i] == new_gpios[j]) {
                gpio_conflict = true;
                break;
            }
        }
    }

    if (gpio_conflict) {
        ESP_LOGW(TAG, "GPIO-Zuordnung mit doppelt vergebenem Pin ignoriert, vorherige Zuordnung bleibt bestehen");
    } else {
        cfg.gpio_relay = new_gpio_relay;
        cfg.gpio_reed = new_gpio_reed;
        cfg.gpio_switch = new_gpio_switch;
        cfg.gpio_pn532_tx = new_gpio_pn532_tx;
        cfg.gpio_pn532_rx = new_gpio_pn532_rx;
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

// -------------------- POST /ota : Firmware-Update --------------------

// Sendet eine einfache Textantwort mit gegebenem HTTP-Status -- fuer
// Fehlerfaelle waehrend des OTA-Uploads, wo httpd_resp_send_err() aus
// esp_http_server nicht alle hier vorkommenden Faelle abdeckt (custom
// Statuscodes/Texte).
static void ota_send_status(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_OK;

    if (req->content_len == 0) {
        ota_send_status(req, "400 Bad Request", "Keine Firmware-Datei empfangen");
        return ESP_FAIL;
    }

    // NULL: naechste freie OTA-Partition ausser der gerade laufenden --
    // schlaegt fehl (liefert NULL), wenn das Projekt (noch) keine
    // Custom-Partitionstabelle mit zwei App-Slots verwendet (siehe
    // partitions.csv/README).
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Keine OTA-Partition gefunden -- partitions.csv (siehe README) konfiguriert?");
        ota_send_status(req, "500 Internal Server Error",
                        "Keine OTA-Partition gefunden. Custom-Partitionstabelle "
                        "(partitions.csv, siehe README) konfiguriert und neu geflasht?");
        return ESP_FAIL;
    }

    if (req->content_len > update_partition->size) {
        ESP_LOGE(TAG, "Firmware-Datei (%zu Byte) groesser als OTA-Partition (%" PRIu32 " Byte)",
                 req->content_len, update_partition->size);
        ota_send_status(req, "400 Bad Request", "Firmware-Datei ist groesser als die verfuegbare OTA-Partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin fehlgeschlagen: %s", esp_err_to_name(err));
        ota_send_status(req, "500 Internal Server Error", "esp_ota_begin fehlgeschlagen");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_RECV_BUF_SIZE);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    ESP_LOGI(TAG, "OTA-Upload gestartet: %d Byte -> Partition %s", remaining, update_partition->label);

    while (remaining > 0) {
        int to_read = remaining < OTA_RECV_BUF_SIZE ? remaining : OTA_RECV_BUF_SIZE;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            } else {
                ota_send_status(req, "400 Bad Request", "Upload abgebrochen/Verbindungsfehler");
            }
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write fehlgeschlagen: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            ota_send_status(req, "500 Internal Server Error", "esp_ota_write fehlgeschlagen");
            return ESP_FAIL;
        }

        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        // Haeufigste Ursache: ESP_ERR_OTA_VALIDATE_FAILED -- Datei war keine
        // gueltige, fuer diesen Chip gebaute App-Image (falsche Datei
        // hochgeladen, oder Secure-Boot/Signaturpruefung schlaegt fehl).
        ESP_LOGE(TAG, "esp_ota_end fehlgeschlagen: %s", esp_err_to_name(err));
        ota_send_status(req, "400 Bad Request", "Ungueltiges Firmware-Image (esp_ota_end fehlgeschlagen)");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition fehlgeschlagen: %s", esp_err_to_name(err));
        ota_send_status(req, "500 Internal Server Error", "esp_ota_set_boot_partition fehlgeschlagen");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA-Update erfolgreich, starte in Kuerze neu...");
    ota_send_status(req, "200 OK", "Firmware-Update erfolgreich. Das Geraet startet jetzt neu...");

    // Siehe Kommentar in save_post_handler() -- gleicher Grund.
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
    // War 6144, dann 10240 -- beides reichte nicht mehr. 6144->10240 war fuer
    // CONFIG_HTTPD_MAX_REQ_HDR_LEN (siehe sdkconfig.defaults, 512->4096 Byte):
    // die Header werden von esp_http_server in stackbasierten Puffern
    // geparst, die mit dieser Kconfig-Groesse mitwachsen. 10240->20480 war
    // noetig, weil index_get_handler() (web_config.c) inzwischen selbst
    // gut 6-7KB lokale Puffer auf dem Stack haelt (ein escapter Kopie-Puffer
    // pro Textfeld, ein <select>-Puffer pro QoS-/GPIO-Dropdown, ...) -- ohne
    // diese Erhoehung stuerzte die httpd-Task auf echter Hardware wieder mit
    // "stack overflow in task httpd" ab, diesmal beim Aufruf von GET /
    // (nicht beim Header-Parsing). Bei weiterem Wachstum von
    // index_get_handler() im Zweifel eher hier weiter erhoehen, statt die
    // Puffer wieder zu verkleinern.
    config.stack_size = 20480;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    // Der Default-Header-Puffer (CONFIG_HTTPD_MAX_REQ_HDR_LEN, siehe
    // sdkconfig.defaults) ist mit 512 Byte zu klein fuer moderne Browser
    // (User-Agent, Accept-*, Sec-Fetch-*, Cookies...) zusammen mit dem
    // Basic-Auth-Header -- fuehrt sonst zu "431 Request Header Fields Too
    // Large" schon beim simplen Seitenaufruf.

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

    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
    };
    httpd_register_uri_handler(server, &ota_uri);

    ESP_LOGI(TAG, "Config-WebGUI gestartet auf Port %d", config.server_port);
    return ESP_OK;
}
