/*
 * Relais-Ansteuerung ueber einen einfachen GPIO-Ausgang.
 * Pin frei waehlbar aus den ungenutzten IOs laut Pinout-Diagramm
 * (z.B. IO4, IO5, IO12, IO14, IO15) - hier IO4 als Beispiel gewaehlt.
 */

#include "relay_control.h"

#include <inttypes.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "relay_control";

#define RELAY_GPIO_PIN   GPIO_NUM_4

// Ueber die WebGUI konfigurierbar (siehe relay_control_init()), Default war
// vormals fest 1500ms -- kurzer Impuls zur Vermeidung von Ueberhitzung der Spule.
static uint32_t s_relay_pulse_ms = 1500;

esp_err_t relay_control_init(uint32_t pulse_ms)
{
    s_relay_pulse_ms = pulse_ms;

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
        ESP_LOGI(TAG, "Relais-GPIO initialisiert (Pin %d, Puls %" PRIu32 "ms)", RELAY_GPIO_PIN, s_relay_pulse_ms);
    }
    return err;
}

void relay_control_pulse(void)
{
    ESP_LOGI(TAG, "Relais wird fuer %" PRIu32 " ms aktiviert", s_relay_pulse_ms);
    gpio_set_level(RELAY_GPIO_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(s_relay_pulse_ms));
    gpio_set_level(RELAY_GPIO_PIN, 0);
    ESP_LOGI(TAG, "Relais wieder deaktiviert");
}

void relay_control_set_pulse_ms(uint32_t pulse_ms)
{
    // uint32_t-Zugriffe sind auf ESP32 atomar (4-Byte-aligned) -- kein Mutex
    // noetig, obwohl dies aus dem MQTT-Event-Handler-Task heraus aufgerufen
    // wird, waehrend relay_control_pulse() im card_event_task (main.c) laeuft.
    s_relay_pulse_ms = pulse_ms;
    ESP_LOGI(TAG, "Relais-Pulsdauer per MQTT auf %" PRIu32 "ms gesetzt", pulse_ms);
}
