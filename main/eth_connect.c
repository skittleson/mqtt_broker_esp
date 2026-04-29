/*
 * Ethernet (W5500 SPI) connection helper for ESP-IDF.
 *
 * Only compiled when CONFIG_MQTT_BROKER_ETHERNET is enabled via Kconfig.
 * Initializes a W5500 SPI Ethernet module, acquires an IP via DHCP,
 * and exposes the netif handle for NAPT bridging in main.c.
 *
 * SPI pin assignments are configured via Kconfig (main/Kconfig.projbuild).
 */

#ifdef CONFIG_MQTT_BROKER_ETHERNET

#include "eth_connect.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "eth";

#define ETH_GOT_IP_BIT   BIT0
#define ETH_TIMEOUT_MS    30000

static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static EventGroupHandle_t s_eth_event_group = NULL;
static volatile int s_eth_connected = 0;
static volatile int s_napt_enabled = 0;

/* ---- Event handlers ---- */

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down");
            s_eth_connected = 0;
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            s_eth_connected = 0;
            break;
        default:
            break;
    }
}

static void eth_got_ip_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_eth_connected = 1;
    if (s_eth_event_group) {
        xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    }
}

/* ---- Public API ---- */

esp_err_t eth_init(void)
{
    esp_err_t ret;

    s_eth_event_group = xEventGroupCreate();
    if (!s_eth_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    /* Install GPIO ISR service (required by W5500 interrupt-driven driver) */
    gpio_install_isr_service(0);

    /* Initialize SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_ETH_SPI_MOSI,
        .miso_io_num = CONFIG_ETH_SPI_MISO,
        .sclk_io_num = CONFIG_ETH_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    ret = spi_bus_initialize(CONFIG_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SPI bus initialized (MOSI=%d, MISO=%d, SCLK=%d)",
             CONFIG_ETH_SPI_MOSI, CONFIG_ETH_SPI_MISO, CONFIG_ETH_SPI_SCLK);

    /* Configure SPI device for W5500 */
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_ETH_SPI_CS,
        .queue_size = 20,
    };

    /* Create W5500 MAC */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(CONFIG_ETH_SPI_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = CONFIG_ETH_SPI_INT;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC");
        spi_bus_free(CONFIG_ETH_SPI_HOST);
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
        return ESP_FAIL;
    }

    /* Create W5500 PHY */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;  /* W5500 always uses PHY address 1 */
    phy_config.reset_gpio_num = CONFIG_ETH_SPI_RST;

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create W5500 PHY");
        mac->del(mac);
        spi_bus_free(CONFIG_ETH_SPI_HOST);
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
        return ESP_FAIL;
    }

    /* Install Ethernet driver */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        phy->del(phy);
        mac->del(mac);
        spi_bus_free(CONFIG_ETH_SPI_HOST);
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
        return ret;
    }

    /* Create network interface and attach to Ethernet driver */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        phy->del(phy);
        mac->del(mac);
        spi_bus_free(CONFIG_ETH_SPI_HOST);
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
        return ESP_FAIL;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ret = esp_netif_attach(s_eth_netif, glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Netif attach failed: %s", esp_err_to_name(ret));
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        spi_bus_free(CONFIG_ETH_SPI_HOST);
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
        return ret;
    }

    /* Register event handlers */
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_got_ip_handler, NULL);

    /* Start Ethernet */
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet started, waiting for DHCP (timeout %ds)...",
             ETH_TIMEOUT_MS / 1000);

    /* Wait for IP via DHCP */
    EventBits_t bits = xEventGroupWaitBits(s_eth_event_group, ETH_GOT_IP_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(ETH_TIMEOUT_MS));

    vEventGroupDelete(s_eth_event_group);
    s_eth_event_group = NULL;

    if (!(bits & ETH_GOT_IP_BIT)) {
        ESP_LOGW(TAG, "Ethernet DHCP timeout (%ds) — no IP acquired",
                 ETH_TIMEOUT_MS / 1000);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Ethernet ready");
    return ESP_OK;
}

esp_netif_t *eth_get_netif(void)
{
    return s_eth_netif;
}

int eth_is_connected(void)
{
    return s_eth_connected;
}

int eth_get_ip_str(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || !s_eth_netif || !s_eth_connected) return -1;

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_eth_netif, &ip) != ESP_OK) return -1;

    snprintf(buf, buf_size, "%lu.%lu.%lu.%lu",
             (unsigned long)(ip.ip.addr & 0xFF),
             (unsigned long)((ip.ip.addr >> 8) & 0xFF),
             (unsigned long)((ip.ip.addr >> 16) & 0xFF),
             (unsigned long)((ip.ip.addr >> 24) & 0xFF));
    return 0;
}

int eth_napt_enable(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ESP_LOGW(TAG, "NAPT enable: WiFi AP netif not found");
        return -1;
    }
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = 1;
        ESP_LOGI(TAG, "NAPT enabled — LAN can reach AP subnet");
        return 0;
    }
    ESP_LOGW(TAG, "NAPT enable failed: %s", esp_err_to_name(err));
    return -1;
}

int eth_napt_disable(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ESP_LOGW(TAG, "NAPT disable: WiFi AP netif not found");
        return -1;
    }
    esp_err_t err = esp_netif_napt_disable(ap_netif);
    if (err == ESP_OK) {
        s_napt_enabled = 0;
        ESP_LOGI(TAG, "NAPT disabled — WiFi AP isolated from LAN");
        return 0;
    }
    ESP_LOGW(TAG, "NAPT disable failed: %s", esp_err_to_name(err));
    return -1;
}

int eth_napt_is_enabled(void)
{
    return s_napt_enabled;
}

#endif /* CONFIG_MQTT_BROKER_ETHERNET */
