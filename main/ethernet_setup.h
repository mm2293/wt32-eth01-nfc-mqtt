#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t ethernet_setup_init(void);
bool ethernet_setup_has_ip(void);
