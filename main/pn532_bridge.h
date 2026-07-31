#pragma once

/*
 * Raw-Bridge-Modus: exponiert die PN532-UART (siehe pn532_uart.c:
 * pn532_uart_init()) 1:1 als TCP-Server, byte-transparent in beide
 * Richtungen. Keine Protokoll-Kenntnis -- im Gegensatz zum Managed-Modus
 * (card_event_task in main.c) wird hier NICHT gepollt, kein SAMConfiguration
 * gesendet, keine Karte erkannt. Gedacht fuer direkten Zugriff durch das
 * ha-nfc-addon (per socat auf ein lokales PTY gelegt) mit Standard-NFC-Tools
 * (mfoc, libnfc/nfc-list, ...) oder einen eigenen Treiber, siehe PROTOCOL.md.
 *
 * Nur EIN Client gleichzeitig -- ein zweiter Verbindungsversuch wird
 * abgelehnt, solange der erste noch besteht (der PN532 kennt ohnehin nur
 * einen Master auf der UART).
 */

#include "esp_err.h"
#include <stdint.h>

/* Startet den TCP-Server-Task auf dem angegebenen Port. Setzt voraus, dass
 * pn532_uart_init() (siehe pn532_uart.h) bereits aufgerufen wurde. Kehrt
 * sofort zurueck, der Server laeuft in einem eigenen Task weiter. */
esp_err_t pn532_bridge_start(uint16_t tcp_port);
