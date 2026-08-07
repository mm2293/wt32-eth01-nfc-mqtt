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

    // MQTT: Clean Session -- Verbindungsweite Einstellung (MQTT kennt das
    // nur pro Client, nicht pro Topic). true (Standard, bisheriges
    // Verhalten): der Broker verwirft Subscriptions und noch nicht
    // zugestellte QoS>0-Nachrichten, sobald die Verbindung getrennt wird.
    // false: "Persistent Session" -- der Broker haelt beides ueber
    // Trennungen hinweg vor, bis der Client mit derselben Client-ID wieder
    // verbindet. Braucht dafuer eine feste (nicht-leere) mqtt_client_id,
    // sonst vergibt esp-mqtt bei jedem Verbindungsaufbau eine neue.
    bool     mqtt_clean_session;

    // MQTT: QoS (0-2) je Topic, individuell ueber die WebGUI einstellbar.
    // Bei ESP32->Addon-Topics (raw/apdu_resp/reed_state) ist es die
    // Publish-QoS, bei Addon->ESP32-Topics (apdu_cmd/result/
    // homekey_group_id/relay_pulse_ms/apdu_relay_timeout_ms) die
    // Subscribe-QoS. Defaults entsprechen dem bisherigen fest kodierten
    // Verhalten (siehe mqtt_client_setup.c).
    uint8_t  qos_raw;
    uint8_t  qos_apdu_cmd;
    uint8_t  qos_apdu_resp;
    uint8_t  qos_result;
    uint8_t  qos_homekey_group_id;
    uint8_t  qos_relay_pulse_ms;
    uint8_t  qos_apdu_relay_timeout_ms;
    uint8_t  qos_reed_state;

    // MQTT: Retain-Flag beim Publish -- nur fuer ESP32->Addon-Topics
    // relevant, bei Subscribe-only-Topics bestimmt der Publisher (Addon)
    // das Retain-Flag, nicht die Firmware.
    bool     retain_raw;
    bool     retain_apdu_resp;
    bool     retain_reed_state;

    // Relais
    uint32_t relay_pulse_ms;
    // true: Pulsdauer kommt zur Laufzeit per MQTT (topic_relay_pulse_ms,
    // retained, Payload = Millisekunden als Zahl-String), relay_pulse_ms
    // dient dabei nur als Fallback bis zur ersten empfangenen Nachricht.
    // false: relay_pulse_ms wird fest verwendet (Standardverhalten).
    bool     relay_pulse_via_mqtt;
    char     topic_relay_pulse_ms[APP_CFG_STR_LEN];

    // Reedkontakt (Tuer-/Schlossstatus, siehe reed_contact.c). Fest
    // verdrahtet auf IO2 (Input mit internem Pull-Up, Kontakt gegen GND --
    // geschlossen = LOW). Publiziert retained bei jedem Statuswechsel UND
    // einmalig beim Start, Payload "closed"/"open".
    char     topic_reed_state[APP_CFG_STR_LEN];

    // Lock-Control (siehe lock_control.c): haelt das Relais nach einem
    // granted:true-Zutrittsvorgang so lange aktiv, wie der Reedkontakt
    // "nicht geschlossen" meldet, plus dieser Nachlaufzeit nach dem
    // Wiederschliessen.
    uint32_t lock_settle_delay_ms;
    // Analog relay_pulse_via_mqtt: true = Nachlaufzeit kommt zur Laufzeit
    // per MQTT (topic_lock_settle_delay_ms, retained, Payload = ms als
    // Zahl-String), lock_settle_delay_ms dient dabei nur als Fallback bis
    // zur ersten empfangenen Nachricht. false = lock_settle_delay_ms wird
    // fest verwendet.
    bool     lock_settle_delay_via_mqtt;
    char     topic_lock_settle_delay_ms[APP_CFG_STR_LEN];
    uint8_t  qos_lock_settle_ms;
    // Sicherheits-Obergrenze (siehe lock_control.h) -- bewusst NICHT per
    // MQTT ueberschreibbar, nur ueber die WebGUI, damit ein fehlerhaftes/
    // kompromittiertes Addon diesen Ueberhitzungsschutz nicht per MQTT
    // aushebeln kann.
    uint32_t lock_max_hold_ms;

    // WebGUI-Login (HTTP Basic Auth, Benutzername fix "admin")
    char     admin_password[APP_CFG_STR_LEN];

    // PN532-Modus: false (Default) = Managed -- Firmware pollt selbst und
    // relayt HomeKey/DESFire-APDUs per MQTT (siehe card_event_task in
    // main.c), unveraendertes Verhalten. true = Raw-Bridge -- Firmware
    // pollt NICHT selbst, sondern exponiert die PN532-UART 1:1 als
    // TCP-Server (siehe pn532_bridge.c) fuer direkten Zugriff durch das
    // Addon/externe Tools (mfoc, libnfc, ...) per socat. Beide Modi sind
    // exklusiv (ein PN532, eine UART) -- Umschalten erfordert einen
    // Neustart (siehe main.c:app_main()).
    bool     pn532_raw_bridge_mode;
    uint16_t pn532_bridge_tcp_port;
} app_config_t;

/* Laedt die Konfiguration aus NVS. Fehlende/noch nie gespeicherte Werte
 * werden mit den bisherigen Compile-Time-Defaults (DHCP, alter MQTT-Broker,
 * alte Topics, 1500ms Relaispuls, Default-Passwort "admin") aufgefuellt --
 * dadurch verhaelt sich ein frisch geflashtes Geraet weiterhin wie vorher,
 * bis jemand ueber die WebGUI etwas aendert. */
esp_err_t app_config_load(app_config_t *cfg);

/* Persistiert die komplette Konfiguration in NVS (ueberschreibt alle Felder). */
esp_err_t app_config_save(const app_config_t *cfg);
