/*
 * PN532-Kommunikation über UART (HSU Mode)
 *
 * Pin-Belegung (WT32-ETH01):
 *   - TX: IO14 (ESP32) -> RX (PN532)
 *   - RX: IO15 (ESP32) -> TX (PN532) [mit internem Pull-Up]
 */

#include "pn532_uart.h"

#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "pn532_uart";

#define PN532_UART_PORT      UART_NUM_1
#define PN532_UART_TX_PIN    14
#define PN532_UART_RX_PIN    15
#define PN532_UART_BAUDRATE  115200
#define PN532_UART_BUF_SIZE  256

static const uint8_t PN532_ACK_FRAME[] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

esp_err_t pn532_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = PN532_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(PN532_UART_PORT, PN532_UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PN532_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(PN532_UART_PORT, PN532_UART_TX_PIN, PN532_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    gpio_set_pull_mode(PN532_UART_RX_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "PN532 UART (TX=%d, RX=%d) initialisiert", PN532_UART_TX_PIN, PN532_UART_RX_PIN);
    return ESP_OK;
}

static void pn532_send_frame(const uint8_t *tfi_and_data, size_t len)
{
    uart_flush_input(PN532_UART_PORT);

    static const uint8_t wakeup[] = {0x55, 0x55, 0x00, 0x00, 0x00, 0x00};
    uart_write_bytes(PN532_UART_PORT, (const char *)wakeup, sizeof(wakeup));

    uint8_t header[] = {0x00, 0x00, 0xFF};
    uart_write_bytes(PN532_UART_PORT, (const char *)header, sizeof(header));

    uint8_t lcs = (uint8_t)(0x100 - len);
    uint8_t len_field[] = {(uint8_t)len, lcs};
    uart_write_bytes(PN532_UART_PORT, (const char *)len_field, sizeof(len_field));

    uart_write_bytes(PN532_UART_PORT, (const char *)tfi_and_data, len);

    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += tfi_and_data[i];
    uint8_t dcs = (uint8_t)(0x100 - sum);
    uint8_t tail[] = {dcs, 0x00};
    uart_write_bytes(PN532_UART_PORT, (const char *)tail, sizeof(tail));
}

static bool pn532_wait_for_ack(int timeout_ms)
{
    uint8_t buf[sizeof(PN532_ACK_FRAME)];
    int len = uart_read_bytes(PN532_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(timeout_ms));
    if (len != sizeof(PN532_ACK_FRAME)) return false;
    return memcmp(buf, PN532_ACK_FRAME, sizeof(PN532_ACK_FRAME)) == 0;
}

esp_err_t pn532_sam_configuration(void)
{
    uint8_t cmd[] = {0xD4, 0x14, 0x01, 0x00};
    pn532_send_frame(cmd, sizeof(cmd));

    if (!pn532_wait_for_ack(100)) return ESP_FAIL;

    uint8_t response[32];
    uart_read_bytes(PN532_UART_PORT, response, sizeof(response), pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "SAMConfiguration erfolgreich");
    return ESP_OK;
}

esp_err_t pn532_start_auto_poll(void)
{
    // D4 60: InAutoPoll
    // 0xFF: Endlose Versuche
    // 0x01: Poll-Periode (150ms)
    // 0x00: Standard Type A (ISO14443A / MIFARE)
    uint8_t cmd[] = {0xD4, 0x60, 0xFF, 0x01, 0x00};
    pn532_send_frame(cmd, sizeof(cmd));

    if (!pn532_wait_for_ack(100)) {
        ESP_LOGE(TAG, "InAutoPoll: kein ACK erhalten");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t pn532_read_auto_poll_response(pn532_card_t *out_card, uint32_t timeout_ms)
{
    uint8_t first_byte;
    // 1. Warte auf das allererste Byte der Antwort (blockiert ohne CPU-Last)
    int res = uart_read_bytes(PN532_UART_PORT, &first_byte, 1, pdMS_TO_TICKS(timeout_ms));
    if (res <= 0) {
        return ESP_ERR_TIMEOUT;
    }

    // 2. Kurze Pause (30ms), damit der PN532 den Rest des Frames schicken kann
    vTaskDelay(pdMS_TO_TICKS(30));

    // 3. Ermittle verfügbare Bytes im UART-Puffer
    size_t available_bytes = 0;
    uart_get_buffered_data_len(PN532_UART_PORT, &available_bytes);

    uint8_t response[64];
    response[0] = first_byte;

    // 4. Lies den Rest ohne weiteres Warten aus
    int read_bytes = uart_read_bytes(PN532_UART_PORT, &response[1],
                                     (available_bytes < 63) ? available_bytes : 63,
                                     pdMS_TO_TICKS(50));
    int len = 1 + read_bytes;

    if (len < 12) return ESP_ERR_NOT_FOUND;

    // Suche Antwort-Header D5 61
    int idx = -1;
    for (int i = 0; i < len - 1; i++) {
        if (response[i] == 0xD5 && response[i + 1] == 0x61) {
            idx = i;
            break;
        }
    }

    if (idx < 0 || idx + 9 >= len) return ESP_ERR_NOT_FOUND;

    uint8_t num_tags = response[idx + 2];
    if (num_tags == 0) return ESP_ERR_NOT_FOUND;

    out_card->atqa[0] = response[idx + 6];
    out_card->atqa[1] = response[idx + 7];
    out_card->sak = response[idx + 8];

    uint8_t uid_len = response[idx + 9];
    if (uid_len > sizeof(out_card->uid) || idx + 10 + uid_len > len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_card->uid, &response[idx + 10], uid_len);
    out_card->uid_len = uid_len;

    return ESP_OK;
}
