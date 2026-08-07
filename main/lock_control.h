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

/* Wertebereich fuer die Sicherheits-Obergrenze (max_hold_ms). Untere Grenze
 * verhindert eine Konfiguration, die das Relais quasi sofort wieder
 * freigibt, obere Grenze deckt sich mit RELAY_PULSE_MS_MAX. */
#define LOCK_MAX_HOLD_MS_MIN       5000UL   // 5s
#define LOCK_MAX_HOLD_MS_MAX       86400000UL  // 24h

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
 *      unabhaengig davon, wie lange das dauert (bis zur
 *      Sicherheits-Obergrenze, siehe unten).
 *   3. Sobald der Reedkontakt wieder "geschlossen" meldet, wird zusaetzlich
 *      settle_delay_ms abgewartet (Nachlaufzeit), bevor das Relais
 *      tatsaechlich deaktiviert wird. Geht der Reedkontakt waehrend dieser
 *      Nachlaufzeit erneut auf "offen", beginnt Schritt 2 von vorn.
 *
 * Sicherheits-Obergrenze (max_hold_ms): relay_control.c pulst das Relais
 * normalerweise bewusst kurz, um die Spule/den Motor nicht zu ueberlasten.
 * Bleibt der Reedkontakt laenger als max_hold_ms am Stueck "nicht
 * geschlossen" (z.B. Tuer wird laengere Zeit aufgehalten), gibt die
 * Firmware das Relais TROTZDEM frei und loggt einen Fehler, statt die Spule
 * endlos unter Strom zu halten. Das ist ein reiner Hardware-Schutz, kein
 * gewuenschtes Verhalten -- im Log/den Geraete-Logs sichtbar, aber ohne
 * eigenes MQTT-Signal (siehe PROTOCOL.md fuer Details).
 *
 * Laeuft in einer eigenen Task (nicht im MQTT-Event-Handler-Task), da ein
 * Haltevorgang je nach Tuersituation lange dauern kann.
 */
esp_err_t lock_control_init(uint32_t settle_delay_ms, uint32_t max_hold_ms);

/* Signalisiert einen granted:true-Zutrittsvorgang. Nicht blockierend --
 * darf und soll aus dem MQTT-Event-Handler-Task aufgerufen werden (siehe
 * mqtt_client_setup.c:handle_result_message()). */
void lock_control_notify_granted(void);

/* Aendert die Nachlaufzeit zur Laufzeit (z.B. per MQTT), analog
 * relay_control_set_pulse_ms(). Wird NICHT in NVS persistiert. */
void lock_control_set_settle_delay_ms(uint32_t ms);
