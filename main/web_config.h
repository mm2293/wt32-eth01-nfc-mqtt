#pragma once

/*
 * Mini-WebGUI zur Laufzeitkonfiguration (Netzwerk, MQTT, Relais, Login) --
 * siehe app_config.h fuer die persistierten Werte. Aenderungen greifen erst
 * nach einem Neustart (Ethernet/MQTT werden nur beim Boot initialisiert),
 * das Speichern loest deshalb automatisch einen esp_restart() aus.
 *
 * Laeuft als eigener HTTP-Server-Task mit niedrigerer Prioritaet als die
 * NFC-Polling-Task (card_event_task, main.c), damit ein Seitenaufruf niemals
 * eine laufende Kartenerkennung/APDU-Relay-Session verzoegert.
 */

#include "esp_err.h"

/* Startet den Config-HTTP-Server auf Port 80. Muss erst NACH
 * ethernet_setup_init()/vorhandener IP aufgerufen werden. */
esp_err_t web_config_start(void);
