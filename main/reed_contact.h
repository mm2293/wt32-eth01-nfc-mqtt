#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Initialisiert den Reedkontakt-Eingang (IO2, Input mit internem Pull-Up)
 * und startet eine eigene Task, die den Pegel entprellt pollt und bei jedem
 * Statuswechsel (sowie einmalig direkt nach dem Start) den aktuellen Status
 * per mqtt_client_setup_publish_reed_state() veroeffentlicht. */
esp_err_t reed_contact_init(void);

/* Letzter entprellter Status (nicht blockierend, kein Warten auf den
 * naechsten Poll-Zyklus) -- von lock_control.c genutzt, um das Relais
 * so lange gehalten zu halten, bis das Schloss laut Reedkontakt wieder in
 * Schliessposition ist. Vor dem ersten abgeschlossenen Entprell-Zyklus
 * (siehe REED_DEBOUNCE_STABLE_POLLS in reed_contact.c) liefert dies den
 * sicheren Default false ("nicht geschlossen"). */
bool reed_contact_is_closed(void);
