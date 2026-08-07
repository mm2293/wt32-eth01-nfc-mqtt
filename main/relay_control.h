#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Erlaubter Wertebereich fuer die Pulsdauer (WebGUI-Feld "relay_sec" in
 * mqtt_client_setup.c:handle_relay_pulse_ms_message() und
 * web_config.c:save_post_handler()) -- untere Grenze verhindert einen
 * wirkungslosen Nullimpuls, obere Grenze ist nur noch eine grosszuegige
 * Ueberlauf-/Tippfehler-Bremse (24h), keine funktionale Beschraenkung mehr
 * (vormals fest 10000ms/10s).
 */
#define RELAY_PULSE_MS_MIN   50UL
#define RELAY_PULSE_MS_MAX   86400000UL  // 24h

/* pulse_ms: Basis-Pulsdauer in ms (ueber die WebGUI konfigurierbar statt
 * fest kodiert) -- die eigentliche Steuerung des Relais inkl.
 * reedkontakt-bewusstem Halten uebernimmt lock_control.c, siehe dort. */
esp_err_t relay_control_init(uint32_t pulse_ms);

/* Low-Level: Relais-GPIO direkt setzen (nicht blockierend). Von
 * lock_control.c genutzt, um das Relais ueber die reine Basis-Pulsdauer
 * hinaus gehalten zu koennen (z.B. solange die Tuer laut Reedkontakt noch
 * nicht wieder in Schliessposition ist). Meldet den neuen Zustand
 * zusaetzlich per MQTT (siehe mqtt_client_setup_publish_relay_state(),
 * Topic nfc/relay_state). */
void relay_control_set(bool energized);

/* Aendert die Basis-Pulsdauer zur Laufzeit, z.B. wenn das Addon sie per
 * MQTT setzt (siehe app_config_t.relay_pulse_via_mqtt, mqtt_client_setup.c).
 * Wird NICHT in NVS persistiert -- bei Bedarf regelmaessig per MQTT
 * nachgesendet (Topic ist retained) oder ueber die WebGUI dauerhaft
 * konfigurieren. */
void relay_control_set_pulse_ms(uint32_t pulse_ms);

/* Aktuell geltende Basis-Pulsdauer (ms), siehe relay_control_set_pulse_ms(). */
uint32_t relay_control_get_pulse_ms(void);
