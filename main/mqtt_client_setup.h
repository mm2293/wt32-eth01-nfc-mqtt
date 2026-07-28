#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t mqtt_client_setup_init(const char *broker_uri, const char *username, const char *password);
void mqtt_client_setup_publish_card(const uint8_t *uid, uint8_t uid_len, uint8_t sak, const uint8_t atqa[2]);
