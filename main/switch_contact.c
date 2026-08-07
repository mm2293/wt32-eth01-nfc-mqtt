/*
 * Manueller Taster-/Schalter-Eingang, der einen Zutrittsvorgang genau wie
 * ein per MQTT gewaehrter NFC-Zutritt ausloest (siehe
 * switch_contact_init()).
 *
 * Verdrahtung: 2-Draht-Schalter/Taster zwischen dem konfigurierten Pin und
 * GND, Pin als Input mit internem Pull-Up konfiguriert -- geschlossener
 * Kontakt zieht den Pin auf LOW, offener Kontakt laesst ihn durch den
 * Pull-Up auf HIGH.
 *
 * Pin ueber die WebGUI aus app_config.h:APP_CFG_GPIO_POOL waehlbar, Default
 * IO12.
 */

#include "switch_contact.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lock_control.h"

static const char *TAG = "switch_contact";

#define SWITCH_POLL_INTERVAL_MS      50
// Wie REED_DEBOUNCE_STABLE_POLLS in reed_contact.c -- 150ms stabiler Pegel,
// bevor eine Betaetigung als echt gilt.
#define SWITCH_DEBOUNCE_STABLE_POLLS 3

static gpio_num_t s_switch_pin = GPIO_NUM_12;

static void switch_contact_task(void *pvParameters)
{
    int last_triggered_level = -1;  // Sentinel -- verhindert Dauerfeuer, solange der Taster gehalten wird
    int candidate_level = -1;
    int candidate_count = 0;

    while (1) {
        int level = gpio_get_level(s_switch_pin);

        if (level == candidate_level) {
            candidate_count++;
        } else {
            candidate_level = level;
            candidate_count = 1;
        }

        if (candidate_count == SWITCH_DEBOUNCE_STABLE_POLLS && candidate_level != last_triggered_level) {
            last_triggered_level = candidate_level;
            if (candidate_level == 0) {
                ESP_LOGI(TAG, "Betaetigung erkannt, benachrichtige Lock-Control");
                lock_control_notify_granted();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SWITCH_POLL_INTERVAL_MS));
    }
}

esp_err_t switch_contact_init(gpio_num_t pin)
{
    s_switch_pin = pin;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_switch_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreate(switch_contact_task, "switch_contact", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Schalterkontakt initialisiert (Pin %d)", s_switch_pin);
    return ESP_OK;
}
