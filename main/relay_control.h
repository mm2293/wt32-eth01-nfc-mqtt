#pragma once

#include "esp_err.h"

esp_err_t relay_control_init(void);

/* Aktiviert das Relais fuer einen kurzen, festen Impuls (blockierend). */
void relay_control_pulse(void);
