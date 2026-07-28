#pragma once

#include "esp_err.h"

esp_err_t keypad_init(void);

/* Scannt einmalig alle Tasten. Gibt das Zeichen der ersten erkannten
 * gedrueckten Taste zurueck, oder 0 wenn keine Taste gedrueckt ist. */
char keypad_scan(void);
