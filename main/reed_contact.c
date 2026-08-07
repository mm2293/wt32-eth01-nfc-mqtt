/*
 * Reedkontakt am Schloss (Tuer-/Schlossstatus: geschlossen/geoeffnet).
 *
 * Verdrahtung: 2-Draht-Reedkontakt zwischen dem konfigurierten Pin und GND,
 * Pin als Input mit internem Pull-Up konfiguriert -- geschlossener Kontakt
 * (Magnet in Reichweite) zieht den Pin auf LOW, offener Kontakt laesst ihn
 * durch den Pull-Up auf HIGH.
 *
 * Pin ueber die WebGUI aus app_config.h:APP_CFG_GPIO_POOL waehlbar (siehe
 * reed_contact_init()), Default IO2.
 */

#include "reed_contact.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_client_setup.h"

static const char *TAG = "reed_contact";

#define REED_POLL_INTERVAL_MS      50
// 3 aufeinanderfolgende stabile Polls (150ms) muessen denselben Pegel
// liefern, bevor ein Statuswechsel als echt gilt -- Prellen des mechanischen
// Kontakts beim Oeffnen/Schliessen wird so gefiltert (analog keypad.c).
#define REED_DEBOUNCE_STABLE_POLLS 3

static gpio_num_t s_reed_pin = GPIO_NUM_2;

// Von reed_contact_is_closed() gelesen -- bool-Zugriffe sind auf ESP32
// atomar (1-Byte), kein Mutex noetig fuer diesen einfachen Cross-Task-Read.
static volatile bool s_reed_closed = false;

static void reed_contact_task(void *pvParameters)
{
    int last_reported_level = -1;  // Sentinel (kein gueltiger GPIO-Pegel) -- erzwingt den allerersten Publish
    int candidate_level = -1;
    int candidate_count = 0;

    while (1) {
        int level = gpio_get_level(s_reed_pin);

        if (level == candidate_level) {
            candidate_count++;
        } else {
            candidate_level = level;
            candidate_count = 1;
        }

        if (candidate_count == REED_DEBOUNCE_STABLE_POLLS) {
            bool closed = (candidate_level == 0);
            s_reed_closed = closed;

            if (candidate_level != last_reported_level) {
                last_reported_level = candidate_level;
                ESP_LOGI(TAG, "Statuswechsel erkannt: %s", closed ? "geschlossen" : "geoeffnet");
                mqtt_client_setup_publish_reed_state(closed);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(REED_POLL_INTERVAL_MS));
    }
}

bool reed_contact_is_closed(void)
{
    return s_reed_closed;
}

esp_err_t reed_contact_init(gpio_num_t pin)
{
    s_reed_pin = pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_reed_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreate(reed_contact_task, "reed_contact", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Reedkontakt initialisiert (Pin %d)", s_reed_pin);
    return ESP_OK;
}
