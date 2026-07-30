#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "app_config.h"

/* cfg darf nach Rueckkehr aus ethernet_setup_init() freigegeben/wiederverwendet
 * werden -- die benoetigten Werte werden intern kopiert bzw. sofort auf den
 * netif angewendet. */
esp_err_t ethernet_setup_init(const app_config_t *cfg);
bool ethernet_setup_has_ip(void);
