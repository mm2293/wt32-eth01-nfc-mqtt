#include "lock_control.h"

#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "relay_control.h"
#include "reed_contact.h"

static const char *TAG = "lock_control";

// Poll-Intervall, mit dem diese Task auf reed_contact_is_closed() prueft --
// bewusst grob (kein Echtzeit-Anspruch), Statuswechsel-Erkennung/Entprellung
// passiert bereits in reed_contact.c.
#define LOCK_HOLD_POLL_INTERVAL_MS 100

static QueueHandle_t s_grant_queue = NULL;
static uint32_t s_settle_delay_ms = 5000;

// Wartet unbegrenzt, bis reed_contact_is_closed()==true.
static void wait_until_reed_closed(void)
{
    while (!reed_contact_is_closed()) {
        vTaskDelay(pdMS_TO_TICKS(LOCK_HOLD_POLL_INTERVAL_MS));
    }
}

// Wartet delay_ms, bricht aber sofort mit false ab, sobald der Reedkontakt
// zwischendurch wieder "nicht geschlossen" meldet.
// Rueckgabe true: die volle Nachlaufzeit blieb der Reedkontakt stabil "geschlossen".
static bool wait_settle_delay_or_reopen(uint32_t delay_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    while (xTaskGetTickCount() < deadline) {
        if (!reed_contact_is_closed()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(LOCK_HOLD_POLL_INTERVAL_MS));
    }
    return true;
}

static void lock_control_task(void *pvParameters)
{
    while (1) {
        uint8_t dummy;
        if (xQueueReceive(s_grant_queue, &dummy, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t pulse_ms = relay_control_get_pulse_ms();
        ESP_LOGI(TAG, "Zutritt gewaehrt, Relais aktiv (Basis-Puls %" PRIu32 "ms)", pulse_ms);
        relay_control_set(true);
        vTaskDelay(pdMS_TO_TICKS(pulse_ms));

        // Reedkontakt-bewusstes Halten: das Schloss darf erst dann wirklich
        // schliessen (Relais aus), wenn es tatsaechlich in Schliessposition
        // ist (Reedkontakt "geschlossen") UND das ueber die Nachlaufzeit
        // stabil bleibt -- sonst kann das Schloss bei geoeffneter Tuer
        // mechanisch gar nicht einrasten (siehe Erklaerung in lock_control.h).
        // Unbegrenztes Warten -- keine Sicherheits-Obergrenze mehr.
        while (1) {
            wait_until_reed_closed();
            if (wait_settle_delay_or_reopen(s_settle_delay_ms)) {
                break;  // stabil geschlossen ueber die volle Nachlaufzeit -- fertig
            }
            ESP_LOGI(TAG, "Reedkontakt waehrend Nachlaufzeit wieder offen, warte erneut auf 'geschlossen'");
        }

        relay_control_set(false);
        ESP_LOGI(TAG, "Schliessvorgang abgeschlossen, Relais deaktiviert");
    }
}

esp_err_t lock_control_init(uint32_t settle_delay_ms)
{
    s_settle_delay_ms = settle_delay_ms;

    s_grant_queue = xQueueCreate(4, sizeof(uint8_t));
    if (s_grant_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(lock_control_task, "lock_control", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Lock-Control gestartet (Nachlaufzeit %" PRIu32 "ms)", s_settle_delay_ms);
    return ESP_OK;
}

void lock_control_notify_granted(void)
{
    if (s_grant_queue == NULL) {
        ESP_LOGW(TAG, "Lock-Control noch nicht initialisiert, ignoriere granted-Event");
        return;
    }
    uint8_t dummy = 1;
    // Nicht blockierend (Timeout 0): darf aus dem MQTT-Event-Handler-Task
    // aufgerufen werden. Ist die Queue voll (mehrere granted-Events, bevor
    // die Task hinterherkommt), wird das ueberzaehlige Event verworfen --
    // ein laufender Halte-/Nachlaufvorgang deckt ohnehin bereits ab, dass
    // das Schloss am Ende korrekt schliesst.
    xQueueSend(s_grant_queue, &dummy, 0);
}

void lock_control_set_settle_delay_ms(uint32_t ms)
{
    s_settle_delay_ms = ms;
    ESP_LOGI(TAG, "Nachlaufzeit per MQTT auf %" PRIu32 "ms gesetzt", ms);
}
