#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

/* Initialisiert einen manuellen Taster-/Schalter-Eingang (Input mit
 * internem Pull-Up) und startet eine eigene Task, die den Pegel entprellt
 * pollt. Bei jeder erkannten Schliessflanke (Pegel geht auf LOW, z.B. weil
 * jemand den angeschlossenen Taster/Schalter betaetigt) wird
 * lock_control_notify_granted() aufgerufen -- der Vorgang laeuft dann exakt
 * wie ein per MQTT gewaehrter Zutritt ab (Basis-Puls, danach reedkontakt-
 * bewusstes Halten/Nachlaufen, siehe lock_control.c). Die Oeffnungsflanke
 * (Loslassen) loest NICHTS aus.
 * pin: GPIO-Eingang, ueber die WebGUI aus APP_CFG_GPIO_POOL waehlbar (siehe
 * app_config.h:gpio_switch), Default IO12. */
esp_err_t switch_contact_init(gpio_num_t pin);
