/*
 * Ethernet-Initialisierung fuer WT32-ETH01 (LAN8720 PHY).
 * Pin-Konfiguration verifiziert gegen mehrere unabhaengige Quellen
 * (egnor/wt32-eth01, ESPHome, martin-ger/esp32_nat_router, MicroPython-Doku):
 *   MDC:   GPIO23
 *   MDIO:  GPIO18
 *   Clock: GPIO0 (extern eingehend)
 *   Power: GPIO16
 *   PHY-Adresse: 1
 */

#include "ethernet_setup.h"

#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "ethernet_setup";

static esp_eth_handle_t s_eth_handle = NULL;
static bool s_got_ip = false;

// true, sobald eine statische IP konfiguriert wurde -- dann liefert
// ETHERNET_EVENT_CONNECTED bereits die "IP" (kein DHCP-Wartezyklus noetig).
static bool s_static_ip_configured = false;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            if (s_static_ip_configured) {
                // Bei statischer IP kommt kein IP_EVENT_ETH_GOT_IP vom DHCP-
                // Client -- die IP steht bereits vor esp_eth_start() fest.
                s_got_ip = true;
            }
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet Link Down");
            s_got_ip = false;
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_got_ip = true;
    }
}

esp_err_t ethernet_setup_init(const app_config_t *cfg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    if (cfg->hostname[0] != '\0') {
        esp_netif_set_hostname(eth_netif, cfg->hostname);
    }

    s_static_ip_configured = !cfg->net_use_dhcp;
    if (s_static_ip_configured) {
        // DHCP-Client MUSS vor esp_netif_set_ip_info() gestoppt werden (sonst
        // ESP_ERR_ESP_NETIF_DHCPC_START_FAILED wenn er spaeter doch noch
        // laeuft/die IP ueberschreibt).
        esp_netif_dhcpc_stop(eth_netif);

        esp_netif_ip_info_t ip_info = {0};
        bool ip_ok = ip4addr_aton(cfg->net_ip, (ip4_addr_t *)&ip_info.ip) != 0;
        bool gw_ok = ip4addr_aton(cfg->net_gateway, (ip4_addr_t *)&ip_info.gw) != 0;
        bool mask_ok = ip4addr_aton(cfg->net_netmask, (ip4_addr_t *)&ip_info.netmask) != 0;

        if (ip_ok && gw_ok && mask_ok) {
            ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));
            ESP_LOGI(TAG, "Statische IP konfiguriert: %s (GW %s, Maske %s)",
                     cfg->net_ip, cfg->net_gateway, cfg->net_netmask);

            if (cfg->net_dns[0] != '\0') {
                esp_netif_dns_info_t dns_info = {0};
                if (ip4addr_aton(cfg->net_dns, (ip4_addr_t *)&dns_info.ip.u_addr.ip4) != 0) {
                    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
                    esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);
                }
            }
        } else {
            ESP_LOGE(TAG, "Statische IP-Konfiguration ungueltig, falle auf DHCP zurueck");
            s_static_ip_configured = false;
            esp_netif_dhcpc_start(eth_netif);
        }
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_mdc_gpio_num = 23;
    esp32_emac_config.smi_mdio_gpio_num = 18;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 16;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth_handle));

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_LOGI(TAG, "Ethernet-Initialisierung abgeschlossen, warte auf Link/IP...");
    return ESP_OK;
}

bool ethernet_setup_has_ip(void)
{
    return s_got_ip;
}
