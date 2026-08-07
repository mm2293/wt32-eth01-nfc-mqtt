#include "pn532_bridge.h"
#include "pn532_uart.h"

#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "pn532_bridge";

#define BRIDGE_BUF_SIZE 512
// Wie oft uart_to_tcp_task() spaetestens aus dem blockierenden UART-Read
// aufwacht, um session->stop zu pruefen -- betrifft NUR, wie zuegig ein
// Verbindungsende erkannt wird, NICHT die Befehlslatenz: anders als in der
// frueheren Round-Robin-Schleife (siehe Historie unten) wartet diese Seite
// nie auf die TCP-Richtung, ein hoher Wert hier kostet also keine
// Kommando-Latenz mehr.
#define UART_READ_POLL_MS 50

static uint16_t s_tcp_port;

/* Zustand einer einzelnen Client-Verbindung, geteilt zwischen den beiden
 * Pump-Tasks (siehe unten). stop wird von der Seite gesetzt, die zuerst ein
 * Verbindungsende/-fehler bemerkt; die jeweils andere Seite entdeckt das
 * spaetestens beim naechsten eigenen Timeout/Datenempfang. done_sem zaehlt
 * die beendeten Tasks hoch, damit bridge_server_task() sicher warten kann,
 * bis BEIDE Tasks fertig sind, bevor der Socket geschlossen und der naechste
 * Client akzeptiert wird (sonst Use-after-free/Doppel-Close-Risiko). */
typedef struct {
    int client_fd;
    uart_port_t uart_port;
    volatile bool stop;
    SemaphoreHandle_t done_sem;
} bridge_session_t;

/* UART -> TCP: blockiert auf der UART, leitet jeden Brocken sofort per
 * send() weiter. Reagiert auf ankommende PN532-Antworten/ACKs ohne auf die
 * TCP-Richtung zu warten -- das ist der eigentliche Fix gegenueber der
 * frueheren Ein-Task-Round-Robin-Schleife (siehe Git-Historie: "Raw-Bridge:
 * Poll-Intervall von 50ms auf 2ms verkleinern"/dessen kompletter Revert):
 * dort blockierte ein einzelner Task abwechselnd auf UART-Read und
 * TCP-select(), sodass ankommende Host-Kommandos im schlechtesten Fall bis
 * zu BRIDGE_POLL_MS warten mussten, bis der Task ueberhaupt wieder auf die
 * TCP-Seite schaute. Mit getrennten Tasks pro Richtung gibt es dieses
 * Kopf-an-Kopf-Warten strukturell nicht mehr -- und ohne kurze
 * Poll-Intervalle brauchte es auch keine FreeRTOS-Tick-Rate-Erhoehung wie
 * beim vorherigen (revertierten) Versuch. */
static void uart_to_tcp_task(void *pvParameters)
{
    bridge_session_t *session = (bridge_session_t *)pvParameters;
    uint8_t buf[BRIDGE_BUF_SIZE];

    while (!session->stop) {
        int uart_len = uart_read_bytes(session->uart_port, buf, sizeof(buf),
                                        pdMS_TO_TICKS(UART_READ_POLL_MS));
        if (uart_len > 0) {
            if (send(session->client_fd, buf, uart_len, 0) < 0) {
                ESP_LOGI(TAG, "uart_to_tcp_task: Client getrennt (send fehlgeschlagen: errno=%d)", errno);
                break;
            }
        } else if (uart_len < 0) {
            ESP_LOGW(TAG, "uart_to_tcp_task: uart_read_bytes fehlgeschlagen");
            break;
        }
    }

    session->stop = true;
    // Weckt tcp_to_uart_task() sofort aus einem evtl. blockierenden recv()
    // auf (siehe dort) -- ohne das koennte diese Seite hier fertig sein,
    // waehrend die andere auf ewig auf den (nun toten) Client wartet.
    shutdown(session->client_fd, SHUT_RDWR);
    xSemaphoreGive(session->done_sem);
    vTaskDelete(NULL);
}

/* TCP -> UART: blockiert unbegrenzt auf recv() (kein Timeout noetig/sinnvoll
 * -- Host-Kommandos kommen unregelmaessig, ein Timeout wuerde hier nur
 * unnoetige Wachphasen erzeugen), leitet jeden Brocken sofort per
 * uart_write_bytes() weiter. */
static void tcp_to_uart_task(void *pvParameters)
{
    bridge_session_t *session = (bridge_session_t *)pvParameters;
    uint8_t buf[BRIDGE_BUF_SIZE];

    while (!session->stop) {
        int tcp_len = recv(session->client_fd, buf, sizeof(buf), 0);
        if (tcp_len > 0) {
            uart_write_bytes(session->uart_port, (const char *)buf, tcp_len);
        } else if (tcp_len == 0) {
            ESP_LOGI(TAG, "tcp_to_uart_task: Client hat die Verbindung geschlossen");
            break;
        } else {
            if (session->stop) {
                // Erwarteter Rueckweg des shutdown() aus uart_to_tcp_task()
                // oben -- kein eigenstaendiger Fehler.
                break;
            }
            ESP_LOGI(TAG, "tcp_to_uart_task: Client getrennt (recv fehlgeschlagen: errno=%d)", errno);
            break;
        }
    }

    session->stop = true;
    xSemaphoreGive(session->done_sem);
    vTaskDelete(NULL);
}

/* Startet beide Pump-Tasks fuer eine Client-Verbindung und wartet, bis
 * BEIDE sich beendet haben (Verbindungsende/-fehler, siehe oben), bevor sie
 * zurueckkehrt -- danach ist der Socket sicher zum Schliessen frei. */
static void run_bridge_session(int client_fd)
{
    uart_port_t uart_port = pn532_uart_get_port();

    // Reste aus einer vorherigen Sitzung (z.B. ECP-Broadcast-Antworten aus
    // dem Managed-Modus vor dem Umschalten) verwerfen, damit der Client mit
    // einem sauberen Strom startet.
    uart_flush_input(uart_port);

    // Nagle/Delayed-ACK auf dem Bridge-Socket kann bei zeitkritischen
    // Nested-Attack-Tools (mfoc) das Timing zwischen aufeinanderfolgenden
    // Auth-Kommandos stoeren -- ergaenzt das passende nodelay auf der
    // Client-Seite (siehe ha-nfc-addon-Repo: pn532_driver.py/
    // raw_bridge_manager.py).
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    bridge_session_t session = {
        .client_fd = client_fd,
        .uart_port = uart_port,
        .stop = false,
        .done_sem = xSemaphoreCreateCounting(2, 0),
    };
    if (session.done_sem == NULL) {
        ESP_LOGE(TAG, "run_bridge_session: xSemaphoreCreateCounting fehlgeschlagen");
        return;
    }

    // Gleiche Prioritaet wie der Server-Task selbst (siehe
    // pn532_bridge_start()) -- beide sind waehrend einer aktiven Sitzung
    // gleichermassen zeitkritisch, keine soll die andere verdraengen.
    xTaskCreate(uart_to_tcp_task, "bridge_u2t", 4096, &session, 5, NULL);
    xTaskCreate(tcp_to_uart_task, "bridge_t2u", 4096, &session, 5, NULL);

    xSemaphoreTake(session.done_sem, portMAX_DELAY);
    xSemaphoreTake(session.done_sem, portMAX_DELAY);
    vSemaphoreDelete(session.done_sem);
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

        run_bridge_session(client_fd);

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
