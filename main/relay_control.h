#pragma once

#include "esp_err.h"
#include <stdint.h>

/* pulse_ms: Dauer des Relaisimpulses in ms, siehe relay_control_pulse()
 * (ueber die WebGUI konfigurierbar statt fest kodiert). */
esp_err_t relay_control_init(uint32_t pulse_ms);

/* Aktiviert das Relais fuer den in relay_control_init() konfigurierten
 * Impuls (blockierend). */
void relay_control_pulse(void);
