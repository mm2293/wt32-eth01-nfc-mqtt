#pragma once

#include "esp_err.h"
#include <stdint.h>

/* pulse_ms: Dauer des Relaisimpulses in ms, siehe relay_control_pulse()
 * (ueber die WebGUI konfigurierbar statt fest kodiert). */
esp_err_t relay_control_init(uint32_t pulse_ms);

/* Aktiviert das Relais fuer den zuletzt via relay_control_init()/
 * relay_control_set_pulse_ms() gesetzten Impuls (blockierend). */
void relay_control_pulse(void);

/* Aendert die Pulsdauer zur Laufzeit, z.B. wenn das Addon sie per MQTT
 * setzt (siehe app_config_t.relay_pulse_via_mqtt, mqtt_client_setup.c). Wird
 * NICHT in NVS persistiert -- bei Bedarf regelmaessig per MQTT nachgesendet
 * (Topic ist retained) oder ueber die WebGUI dauerhaft konfigurieren. */
void relay_control_set_pulse_ms(uint32_t pulse_ms);
