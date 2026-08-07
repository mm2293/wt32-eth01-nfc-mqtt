#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Wertebereich fuer die Nachlaufzeit (settle_delay_ms), analog
 * RELAY_PULSE_MS_MIN/MAX in relay_control.h. Obere Grenze ist grosszuegig,
 * da eine "Nachlaufzeit" von mehreren Minuten fuer traege Tuerschliesser
 * durchaus sinnvoll sein kann. */
#define LOCK_SETTLE_DELAY_MS_MIN   0UL
#define LOCK_SETTLE_DELAY_MS_MAX   600000UL  // 10min

/*
 * Koordiniert Relais (relay_control.c) und Reedkontakt (reed_contact.c) fuer
 * einen kompletten Zutrittsvorgang:
 *
 *   1. Bei granted:true (siehe lock_control_notify_granted()) wird das
 *      Relais aktiviert und bleibt mindestens die Basis-Pulsdauer
 *      (relay_control_get_pulse_ms()) aktiv -- unveraendertes Verhalten.
 *   2. Danach: solange der Reedkontakt "nicht geschlossen" meldet (das
 *      Schloss also mechanisch noch nicht in Schliessposition ist, z.B.
 *      weil die Tuer noch offen steht), bleibt das Relais WEITER aktiv --
 *      unbegrenzt, bis der Reedkontakt wieder "geschlossen" meldet.
 *   3. Sobald der Reedkontakt wieder "geschlossen" meldet, wird zusaetzlich
 *      settle_delay_ms abgewartet (Nachlaufzeit), bevor das Relais
 *      tatsaechlich deaktiviert wird. Geht der Reedkontakt waehrend dieser
 *      Nachlaufzeit erneut auf "offen", beginnt Schritt 2 von vorn.
 *
 * Laeuft in einer eigenen Task (nicht im MQTT-Event-Handler-Task), da ein
 * Haltevorgang je nach Tuersituation lange dauern kann.
 *
 * reed_enabled: false, wenn app_config.h:reed_enabled deaktiviert ist (kein
 * Reedkontakt angeschlossen/reed_contact_init() nie aufgerufen) -- die
 * Schritte 2+3 werden dann komplett uebersprungen, sonst wuerde das Relais
 * nach jedem Zutritt fuer immer aktiv bleiben (reed_contact_is_closed()
 * liefert ohne laufende Task nie true). Verhaelt sich dann wie vor
 * Einfuehrung der Reedkontakt-Logik: nur der Basis-Puls. */
esp_err_t lock_control_init(uint32_t settle_delay_ms, bool reed_enabled);

/* Signalisiert einen granted:true-Zutrittsvorgang. Nicht blockierend --
 * darf und soll aus dem MQTT-Event-Handler-Task aufgerufen werden (siehe
 * mqtt_client_setup.c:handle_result_message()). */
void lock_control_notify_granted(void);

/* Aendert die Nachlaufzeit zur Laufzeit (z.B. per MQTT), analog
 * relay_control_set_pulse_ms(). Wird NICHT in NVS persistiert. */
void lock_control_set_settle_delay_ms(uint32_t ms);
