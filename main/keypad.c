/*
 * Keypad-Modul (Grundgerüst fuer spaetere Nutzung).
 * Vorgesehen fuer ein 4x3 Matrix-Keypad (Ziffern 0-9 plus * und #).
 *
 * WICHTIG: Dies ist ein GRUNDGERUEST - die tatsaechliche Pin-Zuordnung
 * (welche IOs fuer Zeilen/Spalten genutzt werden) muss noch anhand der
 * dann tatsaechlich noch freien GPIO-Pins am WT32-ETH01 festgelegt werden,
 * da PN532 (UART) und Relais bereits mehrere Pins belegen. Ein Matrix-
 * Keypad mit 4 Zeilen + 3 Spalten braucht 7 zusaetzliche GPIO-Pins.
 *
 * Laut Pinout-Diagramm noch potenziell frei: IO39, IO36, IO15, IO14,
 * IO12, IO5, IO2 (7 Pins - passt exakt fuer ein 4x3-Keypad, IO4 ist
 * ja schon fuers Relais reserviert). IO34-39 sind nur Input-faehig,
 * was fuer Zeilen (die gelesen werden) gut passt, aber NICHT fuer
 * Spalten (die aktiv angesteuert werden muessen) - das muss beim
 * finalen Pin-Mapping beachtet werden!
 */

#include "keypad.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "keypad";

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 3

// TODO: Pin-Zuordnung finalisieren, sobald klar ist welche IOs frei bleiben.
// Platzhalter-Werte, NICHT ungeprueft uebernehmen:
static const gpio_num_t row_pins[KEYPAD_ROWS] = {GPIO_NUM_36, GPIO_NUM_39, GPIO_NUM_34, GPIO_NUM_35};
static const gpio_num_t col_pins[KEYPAD_COLS] = {GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_15};

static const char keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'},
};

esp_err_t keypad_init(void)
{
    // Spalten als Ausgaenge konfigurieren (werden nacheinander auf LOW gesetzt)
    for (int c = 0; c < KEYPAD_COLS; c++) {
        gpio_config_t col_conf = {
            .pin_bit_mask = (1ULL << col_pins[c]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&col_conf);
        gpio_set_level(col_pins[c], 1);  // Standard: HIGH (inaktiv)
    }

    // Zeilen als Eingaenge mit Pull-Up konfigurieren
    for (int r = 0; r < KEYPAD_ROWS; r++) {
        gpio_config_t row_conf = {
            .pin_bit_mask = (1ULL << row_pins[r]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&row_conf);
    }

    ESP_LOGI(TAG, "Keypad initialisiert (Platzhalter-Pinbelegung, siehe TODO im Code)");
    return ESP_OK;
}

char keypad_scan(void)
{
    for (int c = 0; c < KEYPAD_COLS; c++) {
        gpio_set_level(col_pins[c], 0);  // aktuelle Spalte aktivieren (LOW)

        for (int r = 0; r < KEYPAD_ROWS; r++) {
            if (gpio_get_level(row_pins[r]) == 0) {
                // Taste gedrueckt: kurz entprellen, dann warten bis losgelassen
                vTaskDelay(pdMS_TO_TICKS(20));
                while (gpio_get_level(row_pins[r]) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                gpio_set_level(col_pins[c], 1);
                return keymap[r][c];
            }
        }

        gpio_set_level(col_pins[c], 1);  // Spalte wieder deaktivieren
    }

    return 0;  // keine Taste gedrueckt
}
