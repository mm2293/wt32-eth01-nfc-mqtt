#pragma once

/*
 * Zentrale, per NVS persistierte Konfiguration (Netzwerk, MQTT, Relais,
 * WebGUI-Login). Ersetzt die vormals in main.c/mqtt_client_setup.c/
 * relay_control.c fest verdrahteten Werte, damit sie zur Laufzeit ueber die
 * Mini-WebGUI (siehe web_config.c) geaendert werden koennen.
 *
 * Alle Textfelder sind feste Puffer (kein malloc), damit die Struktur
 * unproblematisch auf dem Stack liegen kann.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define APP_CFG_STR_LEN   64
#define APP_CFG_IP_LEN    16

typedef struct {
    // Netzwerk
    bool     net_use_dhcp;
    char     net_ip[APP_CFG_IP_LEN];
    char     net_gateway[APP_CFG_IP_LEN];
    char     net_netmask[APP_CFG_IP_LEN];
    char     net_dns[APP_CFG_IP_LEN];
    char     hostname[APP_CFG_STR_LEN];

    // MQTT
    char     mqtt_broker_uri[APP_CFG_STR_LEN];
    char     mqtt_username[APP_CFG_STR_LEN];
    char     mqtt_password[APP_CFG_STR_LEN];
    char     mqtt_client_id[APP_CFG_STR_LEN];
    char     topic_raw[APP_CFG_STR_LEN];
    char     topic_apdu_cmd[APP_CFG_STR_LEN];
    char     topic_apdu_resp[APP_CFG_STR_LEN];
    char     topic_result[APP_CFG_STR_LEN];
    char     topic_homekey_group_id[APP_CFG_STR_LEN];

    // Relais
    uint32_t relay_pulse_ms;

    // WebGUI-Login (HTTP Basic Auth, Benutzername fix "admin")
    char     admin_password[APP_CFG_STR_LEN];
} app_config_t;

/* Laedt die Konfiguration aus NVS. Fehlende/noch nie gespeicherte Werte
 * werden mit den bisherigen Compile-Time-Defaults (DHCP, alter MQTT-Broker,
 * alte Topics, 1500ms Relaispuls, Default-Passwort "admin") aufgefuellt --
 * dadurch verhaelt sich ein frisch geflashtes Geraet weiterhin wie vorher,
 * bis jemand ueber die WebGUI etwas aendert. */
esp_err_t app_config_load(app_config_t *cfg);

/* Persistiert die komplette Konfiguration in NVS (ueberschreibt alle Felder). */
esp_err_t app_config_save(const app_config_t *cfg);
