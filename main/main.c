/*
 * ESP32 MQTT Broker — Application Entry Point
 *
 * Initializes NVS, WiFi, LED status, captive portal, and starts the
 * MQTT broker.
 */

#include "mqtt_broker.h"
#include "wifi_connect.h"
#include "portal.h"
#include "ntp.h"
#include "led_strip.h"
#include "mdns.h"

#ifdef CONFIG_MQTT_BROKER_ETHERNET
#include "eth_connect.h"
#include "esp_netif.h"
#include "nvs.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";

/* ---- LED status patterns ---- */

static led_strip_handle_t s_led = NULL;

static void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = GPIO_NUM_21,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed: %d", err);
        s_led = NULL;
    }
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

static void led_off(void)
{
    if (!s_led) return;
    led_strip_clear(s_led);
}

/* Blue fast blink — booting */
static void led_boot_pattern(void)
{
    for (int i = 0; i < 6; i++) {
        led_set(0, 0, 30);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Yellow 2-blink — connecting WiFi */
static void led_connecting_pattern(void)
{
    for (int i = 0; i < 2; i++) {
        led_set(30, 20, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* Red 3-blink — WiFi failed, AP mode */
static void led_fail_pattern(void)
{
    for (int i = 0; i < 3; i++) {
        led_set(30, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* LED status task — runs continuously after startup */
typedef enum {
    LED_STATE_BOOT,
    LED_STATE_CONNECTING,
    LED_STATE_FAILED_AP,
    LED_STATE_CONNECTED,
    LED_STATE_AP_ONLY,
    LED_STATE_ETH_CONNECTED,  /* Ethernet + WiFi AP gateway mode */
} led_state_t;

static volatile led_state_t s_led_state = LED_STATE_BOOT;

static void led_task(void *arg)
{
    while (1) {
        switch (s_led_state) {
            case LED_STATE_BOOT:
                led_boot_pattern();
                break;
            case LED_STATE_CONNECTING:
                led_connecting_pattern();
                break;
            case LED_STATE_FAILED_AP:
                led_fail_pattern();
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case LED_STATE_CONNECTED:
                /* Green slow pulse */
                led_set(0, 20, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case LED_STATE_AP_ONLY:
                /* Cyan slow pulse */
                led_set(0, 15, 20);
                vTaskDelay(pdMS_TO_TICKS(1000));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case LED_STATE_ETH_CONNECTED:
                /* White slow pulse — Ethernet gateway mode */
                led_set(20, 20, 20);
                vTaskDelay(pdMS_TO_TICKS(1000));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- Entry point ---- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 MQTT Broker starting ===");

    /* Initialize NVS (required for WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize LED */
    led_init();
    s_led_state = LED_STATE_BOOT;

    /* Start LED status task */
    xTaskCreate(led_task, "led_task", 2048, NULL, 2, NULL);

    /* Connect WiFi (blocks until connected or falls back to AP) */
    s_led_state = LED_STATE_CONNECTING;
    ESP_LOGI(TAG, "Connecting WiFi...");
    int wifi_ok = wifi_connect_sta();

    if (wifi_ok == 0 && wifi_get_sta_connected()) {
        ESP_LOGI(TAG, "WiFi STA connected");
        s_led_state = LED_STATE_CONNECTED;
    } else {
        ESP_LOGW(TAG, "WiFi STA not connected, running in AP mode");
        s_led_state = LED_STATE_FAILED_AP;
    }

    /* Start captive portal (for WiFi config via web browser).
     * Always enable AP so the portal is reachable at the AP IP,
     * even when STA is connected (AP+STA mode). */
    ESP_LOGI(TAG, "Starting captive portal...");
    if (wifi_get_sta_connected()) {
        /* STA connected — switch to AP+STA so portal is reachable */
        wifi_set_ap_mode(1);
    }
    portal_start();
    if (!wifi_get_sta_connected()) {
        s_led_state = LED_STATE_AP_ONLY;
    }

    /* Initialize Ethernet gateway if enabled at build time */
#ifdef CONFIG_MQTT_BROKER_ETHERNET
    ESP_LOGI(TAG, "Initializing Ethernet (W5500 SPI)...");
    if (eth_init() == ESP_OK) {
        /* Ethernet is up — prefer it as the LAN uplink. Drop the WiFi STA
         * so we don't hold a second DHCP lease on the same router. The AP
         * stays up so the captive portal remains reachable. */
        if (wifi_get_sta_connected()) {
            ESP_LOGI(TAG, "Ethernet up — disconnecting WiFi STA");
            wifi_stop_sta();
        }

        /* With STA gone, NAPT bridging from LAN → AP subnet is the only
         * way AP clients reach the internet. Check NVS for saved state
         * (default: enabled). */
        uint8_t napt_en = 1;
        {
            nvs_handle_t h;
            if (nvs_open("mqtt_cfg", NVS_READONLY, &h) == ESP_OK) {
                nvs_get_u8(h, "napt_en", &napt_en);
                nvs_close(h);
            }
        }

        if (napt_en) {
            eth_napt_enable();
        } else {
            ESP_LOGI(TAG, "NAPT disabled by saved config — WiFi AP isolated");
        }

        char eth_ip[16] = "";
        eth_get_ip_str(eth_ip, sizeof(eth_ip));
        ESP_LOGI(TAG, "Ethernet gateway ready — LAN IP: %s, NAPT: %s",
                 eth_ip, napt_en ? "on" : "off");
        s_led_state = LED_STATE_ETH_CONNECTED;
    } else {
        ESP_LOGW(TAG, "Ethernet init failed — continuing WiFi-only");
    }
#endif

    /* Start mDNS (advertises <hostname>.local + _mqtt._tcp / _http._tcp).
     * Safe to start even in AP-only mode — it'll bind to whichever
     * netifs are up. */
    {
        char host[33] = "";
        portal_get_hostname(host, sizeof(host));
        if (host[0]) {
            esp_err_t mret = mdns_init();
            if (mret == ESP_OK) {
                mdns_hostname_set(host);
                mdns_instance_name_set("ESP32 MQTT Broker");
                mdns_service_add(NULL, "_mqtt", "_tcp", 1883, NULL, 0);
                mdns_service_add(NULL, "_http", "_tcp", 80,   NULL, 0);
                /* SNTP service advertisement (Phase 3 of plan-ntp-server.md).
                 * Substitutes for DHCP option 42 which ESP-IDF's DHCP server
                 * doesn't expose for arbitrary option codes -- it only emits
                 * router and DNS from the dhcps_offer_option enum. mDNS
                 * advertising _ntp._udp gets the same auto-discovery on
                 * Avahi-aware clients (macOS, iOS, ChromeOS, Linux). For
                 * Windows / dumb embedded clients the user still needs to
                 * point the NTP source at <hostname>.local or the IP. */
                mdns_service_add(NULL, "_ntp",  "_udp", 123,  NULL, 0);
                ESP_LOGI(TAG, "mDNS started: %s.local (mqtt/http/ntp)", host);
            } else {
                ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(mret));
            }
        }
    }

    /* Start MQTT broker */
    ESP_LOGI(TAG, "Starting MQTT broker...");
    broker_start();

    /* Start SNTP client + server (Phases 1 and 2 of plan-ntp-server.md).
     * Both honour NVS namespace `ntp`: `enabled` is the master switch,
     * `srv_enabled` independently disables the server alone (useful for
     * STA-only nodes that just want their own clock synced). The server
     * is safe to start before the client has synced -- it answers with
     * stratum 16 / LI=3 (alarm) until real time is available, which
     * RFC-compliant clients ignore. */
    ESP_LOGI(TAG, "Starting SNTP client + server...");
    ntp_init();
    ntp_server_start();

    ESP_LOGI(TAG, "=== ESP32 MQTT Broker ready ===");
}
