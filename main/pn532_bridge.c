#include "pn532_bridge.h"
#include "pn532_uart.h"

#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "pn532_bridge";

#define BRIDGE_BUF_SIZE 512
// Kurze Poll-Timeouts auf beiden Seiten (UART-Read, select() auf dem Socket)
// statt langem Blockieren -- damit ein Verbindungsabbruch zuegig erkannt und
// der Server-Loop fuer den naechsten Client wieder frei wird.
#define BRIDGE_POLL_MS 50

static uint16_t s_tcp_port;

/* Pumpt Rohbytes bidirektional zwischen der PN532-UART und dem verbundenen
 * TCP-Client, bis eine Seite die Verbindung schliesst oder ein Fehler
 * auftritt. Keine Interpretation der Bytes -- reine Bruecke, siehe
 * pn532_bridge.h. */
static void pump_bytes(int client_fd)
{
    uart_port_t uart_port = pn532_uart_get_port();
    uint8_t buf[BRIDGE_BUF_SIZE];

    // Reste aus einer vorherigen Sitzung (z.B. ECP-Broadcast-Antworten aus
    // dem Managed-Modus vor dem Umschalten) verwerfen, damit der Client mit
    // einem sauberen Strom startet.
    uart_flush_input(uart_port);

    while (1) {
        // UART -> TCP
        int uart_len = uart_read_bytes(uart_port, buf, sizeof(buf), pdMS_TO_TICKS(BRIDGE_POLL_MS));
        if (uart_len > 0) {
            int sent = send(client_fd, buf, uart_len, 0);
            if (sent < 0) {
                ESP_LOGI(TAG, "Client getrennt (send fehlgeschlagen: errno=%d)", errno);
                return;
            }
        } else if (uart_len < 0) {
            ESP_LOGW(TAG, "uart_read_bytes fehlgeschlagen");
            return;
        }

        // TCP -> UART: kurzer select() statt blockierendem recv(), damit
        // diese Schleife weiterhin regelmaessig auch die UART-Richtung
        // bedient, statt beliebig lange auf Client-Daten zu warten.
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = BRIDGE_POLL_MS * 1000 };

        int sel = select(client_fd + 1, &read_fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(client_fd, &read_fds)) {
            int tcp_len = recv(client_fd, buf, sizeof(buf), 0);
            if (tcp_len > 0) {
                uart_write_bytes(uart_port, (const char *)buf, tcp_len);
            } else if (tcp_len == 0) {
                ESP_LOGI(TAG, "Client hat die Verbindung geschlossen");
                return;
            } else {
                ESP_LOGI(TAG, "Client getrennt (recv fehlgeschlagen: errno=%d)", errno);
                return;
            }
        } else if (sel < 0) {
            ESP_LOGW(TAG, "select() fehlgeschlagen: errno=%d", errno);
            return;
        }
    }
}

static void bridge_server_task(void *pvParameters)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() fehlgeschlagen: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(s_tcp_port),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() auf Port %u fehlgeschlagen: errno=%d", s_tcp_port, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen() fehlgeschlagen: errno=%d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Raw-Bridge-TCP-Server laeuft auf Port %u", s_tcp_port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        // Nur EIN Client gleichzeitig (siehe pn532_bridge.h) -- blockierendes
        // accept() ist hier in Ordnung, in dieser Zeit ist die UART ohnehin
        // frei/ungenutzt.
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            ESP_LOGW(TAG, "accept() fehlgeschlagen: errno=%d", errno);
            continue;
        }

        char ip_str[16];
        inet_ntoa_r(client_addr.sin_addr, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Client verbunden: %s:%d", ip_str, ntohs(client_addr.sin_port));

        // Nagle deaktivieren: mfocs Nested-Attack ist auf enges Timing
        // zwischen aufeinanderfolgenden Auth-Kommandos angewiesen (siehe
        // raw_bridge_manager.py im ha-nfc-addon-Repo) -- die Verzoegerung,
        // die Nagle+Delayed-ACK sonst je nach Paketgroesse einstreut, reicht
        // aus, um den Nonce-Request beim Sektor-Angriff scheitern zu lassen
        // ("Error requesting encrypted tag-nonce").
        int nodelay_opt = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay_opt, sizeof(nodelay_opt)) != 0) {
            ESP_LOGW(TAG, "setsockopt(TCP_NODELAY) fehlgeschlagen: errno=%d", errno);
        }

        pump_bytes(client_fd);

        close(client_fd);
        ESP_LOGI(TAG, "Client-Verbindung beendet, warte auf naechste Verbindung");
    }
}

esp_err_t pn532_bridge_start(uint16_t tcp_port)
{
    s_tcp_port = tcp_port;
    // Gleiche Prioritaet wie card_event_task im Managed-Modus (main.c) --
    // beide laufen nie gleichzeitig (siehe app_config_t:pn532_raw_bridge_mode),
    // aber so bleibt die Bruecke im laufenden Betrieb ebenso reaktionsfreudig.
    BaseType_t ok = xTaskCreate(bridge_server_task, "pn532_bridge", 4096, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
