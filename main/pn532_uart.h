#ifndef PN532_UART_H
#define PN532_UART_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t sak;
    uint8_t atqa[2];
} pn532_card_t;

esp_err_t pn532_uart_init(void);
esp_err_t pn532_sam_configuration(void);
esp_err_t pn532_start_auto_poll(void);
esp_err_t pn532_read_auto_poll_response(pn532_card_t *out_card, uint32_t timeout_ms);

#endif
