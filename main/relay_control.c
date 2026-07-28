/*
 * Relais-Ansteuerung ueber einen einfachen GPIO-Ausgang.
 * Pin frei waehlbar aus den ungenutzten IOs laut Pinout-Diagramm
 * (z.B. IO4, IO5, IO12, IO14, IO15) - hier IO4 als Beispiel gewaehlt.
 */

#include "relay_control.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "relay_control";

#define RELAY_GPIO_PIN   GPIO_NUM_4
#define RELAY_PULSE_MS   1500   // kurzer Impuls, siehe Hauptsession-Diskussion
                                  // zur Vermeidung von Ueberhitzung der Spule

esp_err_t relay_control_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELAY_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // definierter LOW-Zustand beim Start
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err == ESP_OK) {
        gpio_set_level(RELAY_GPIO_PIN, 0);
        ESP_LOGI(TAG, "Relais-GPIO initialisiert (Pin %d)", RELAY_GPIO_PIN);
    }
    return err;
}

void relay_control_pulse(void)
{
    ESP_LOGI(TAG, "Relais wird fuer %d ms aktiviert", RELAY_PULSE_MS);
    gpio_set_level(RELAY_GPIO_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(RELAY_PULSE_MS));
    gpio_set_level(RELAY_GPIO_PIN, 0);
    ESP_LOGI(TAG, "Relais wieder deaktiviert");
}
