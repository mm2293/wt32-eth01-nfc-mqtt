#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>

/* Initialisiert den Reedkontakt-Eingang (Input mit internem Pull-Up) und
 * startet eine eigene Task, die den Pegel entprellt pollt und bei jedem
 * Statuswechsel (sowie einmalig direkt nach dem Start) den aktuellen Status
 * per mqtt_client_setup_publish_reed_state() veroeffentlicht.
 * pin: GPIO-Eingang, ueber die WebGUI aus APP_CFG_GPIO_POOL waehlbar (siehe
 * app_config.h:gpio_reed), Default IO2. */
esp_err_t reed_contact_init(gpio_num_t pin);

/* Letzter entprellter Status (nicht blockierend, kein Warten auf den
 * naechsten Poll-Zyklus) -- von lock_control.c genutzt, um das Relais
 * so lange gehalten zu halten, bis das Schloss laut Reedkontakt wieder in
 * Schliessposition ist. Vor dem ersten abgeschlossenen Entprell-Zyklus
 * (siehe REED_DEBOUNCE_STABLE_POLLS in reed_contact.c) liefert dies den
 * sicheren Default false ("nicht geschlossen"). */
bool reed_contact_is_closed(void);
