/*
 * WiFi STA/AP connection helper for ESP-IDF.
 *
 * Supports:
 *   - Loading/saving WiFi credentials from NVS
 *   - STA mode, AP mode, and AP+STA mode
 *   - Automatic AP fallback when STA connection fails
 *   - Storing/clearing credentials in NVS
 *   - ESP32-S3 v0.2 silicon bug workaround
 */

#include "wifi_connect.h"
#include "portal.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define WIFI_MAX_RETRIES     20
#define NVS_NAMESPACE        "mqtt_wifi"

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static int s_wifi_mode = WIFI_MODE_STA;
static int s_ap_enabled = 1;

static esp_event_handler_instance_t s_inst_any_id;
static esp_event_handler_instance_t s_inst_got_ip;

/* ---- NVS helpers ---- */

static esp_err_t nvs_get_sta_ssid(char *ssid, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t len = max_len;
    err = nvs_get_str(handle, "ssid", ssid, &len);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_get_sta_password(char *password, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t len = max_len;
    err = nvs_get_str(handle, "password", password, &len);
    nvs_close(handle);
    return err;
}

static int nvs_has_credentials(void)
{
    char ssid[33] = "";
    size_t ssid_len = sizeof(ssid);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return 0;
    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    nvs_close(handle);
    return (err == ESP_OK && ssid_len > 0);
}

static esp_err_t nvs_save_sta_config(const char *ssid, const char *password, int ap_mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", password);
    if (err == ESP_OK) {
        uint8_t val = (uint8_t)(ap_mode ? 1 : 0);
        err = nvs_set_u8(handle, "ap_mode", val);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_clear_sta_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    nvs_close(handle);
    return err;
}

static int nvs_get_ap_mode(void)
{
    nvs_handle_t handle;
    uint8_t val = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return 0;
    err = nvs_get_u8(handle, "ap_mode", &val);
    nvs_close(handle);
    return (err == ESP_OK) ? (int)val : 0;
}

/* ---- WiFi event handler (v0.2 compatible — register BEFORE wifi_init) ---- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_count++;
        if (s_retry_count < WIFI_MAX_RETRIES) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---- STA connection ---- */

static int wifi_init_sta(const char *ssid, const char *password)
{
    if (s_wifi_event_group) vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = xEventGroupCreate();
    s_retry_count = 0;

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    if (ssid) {
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    }
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid ? ssid : "(none)");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(60000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        esp_wifi_stop();
    }
    /* On success, leave WiFi running — caller depends on the connection staying up */
    return (bits & WIFI_CONNECTED_BIT) ? 0 : -1;
}

/* ---- AP configuration helpers ---- */

#define NVS_SETTINGS_NS  "mqtt_cfg"

/* Default AP IP from Kconfig (or fallback if not defined) */
#ifndef CONFIG_MQTT_BROKER_AP_IP
#define CONFIG_MQTT_BROKER_AP_IP      "192.168.25.1"
#endif
#ifndef CONFIG_MQTT_BROKER_AP_NETMASK
#define CONFIG_MQTT_BROKER_AP_NETMASK "255.255.255.0"
#endif

static void load_ap_ip(char *ip_out, size_t ip_size, char *mask_out, size_t mask_size)
{
    /* Defaults from Kconfig */
    strncpy(ip_out, CONFIG_MQTT_BROKER_AP_IP, ip_size - 1);
    ip_out[ip_size - 1] = '\0';
    strncpy(mask_out, CONFIG_MQTT_BROKER_AP_NETMASK, mask_size - 1);
    mask_out[mask_size - 1] = '\0';

    /* Override from NVS if saved */
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = ip_size;
        if (nvs_get_str(h, "ap_ip", ip_out, &len) == ESP_OK && len > 1) {
            ESP_LOGI(TAG, "Loaded AP IP from NVS: '%s'", ip_out);
        }
        len = mask_size;
        if (nvs_get_str(h, "ap_mask", mask_out, &len) == ESP_OK && len > 1) {
            ESP_LOGI(TAG, "Loaded AP netmask from NVS: '%s'", mask_out);
        }
        nvs_close(h);
    }
}

/*
 * Apply custom AP IP/netmask to an AP netif.
 * Must be called after netif creation but before or right after esp_wifi_start().
 * Stops the DHCP server, sets IP, restarts DHCP server.
 */
static void apply_ap_ip(esp_netif_t *ap_netif)
{
    if (!ap_netif) return;

    char ip_str[16], mask_str[16];
    load_ap_ip(ip_str, sizeof(ip_str), mask_str, sizeof(mask_str));

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = ipaddr_addr(ip_str);
    ip_info.gw.addr = ipaddr_addr(ip_str);   /* gateway = AP itself */
    ip_info.netmask.addr = ipaddr_addr(mask_str);

    /* Must stop DHCP server before changing IP */
    esp_netif_dhcps_stop(ap_netif);
    esp_err_t err = esp_netif_set_ip_info(ap_netif, &ip_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "AP IP set to %s (mask %s)", ip_str, mask_str);
    } else {
        ESP_LOGW(TAG, "Failed to set AP IP: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(ap_netif);
}

static void load_ap_config(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    /* Defaults */
    strncpy(ssid, "mqtt-broker", ssid_size - 1);
    ssid[ssid_size - 1] = '\0';
    strncpy(pass, "mqtt1234", pass_size - 1);
    pass[pass_size - 1] = '\0';

    /* Try to load from NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = ssid_size;
        if (nvs_get_str(h, "ap_ssid", ssid, &len) == ESP_OK && len > 1) {
            ESP_LOGI(TAG, "Loaded AP SSID from NVS: '%s'", ssid);
        }
        len = pass_size;
        if (nvs_get_str(h, "ap_pass", pass, &len) == ESP_OK && len > 1) {
            ESP_LOGI(TAG, "Loaded AP password from NVS (len=%d)", (int)(len - 1));
        }
        nvs_close(h);
    }
}

static void apply_ap_config(wifi_config_t *ap_config)
{
    char ap_ssid[33], ap_pass[65];
    load_ap_config(ap_ssid, sizeof(ap_ssid), ap_pass, sizeof(ap_pass));

    memset(ap_config, 0, sizeof(*ap_config));
    size_t ssid_len = strlen(ap_ssid);
    memcpy(ap_config->ap.ssid, ap_ssid, ssid_len);
    ap_config->ap.ssid_len = (uint8_t)ssid_len;
    memcpy(ap_config->ap.password, ap_pass, strlen(ap_pass));
    ap_config->ap.channel = 1;
    ap_config->ap.max_connection = 8;
    ap_config->ap.authmode = (strlen(ap_pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

static int wifi_init_ap(void)
{
    wifi_config_t ap_config;
    apply_ap_config(&ap_config);

    ESP_ERROR_CHECK(esp_wifi_set_mode(s_wifi_mode));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP '%s' started", (char *)ap_config.ap.ssid);
    return 0;
}

/* ---- Public API ---- */

int wifi_connect_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    /* Apply configured hostname before DHCP starts negotiating. */
    {
        char host[33] = "";
        portal_get_hostname(host, sizeof(host));
        if (sta_netif && host[0]) {
            esp_err_t herr = esp_netif_set_hostname(sta_netif, host);
            if (herr == ESP_OK) {
                ESP_LOGI(TAG, "STA hostname set: '%s'", host);
            } else {
                ESP_LOGW(TAG, "esp_netif_set_hostname failed: %s", esp_err_to_name(herr));
            }
        }
    }

    /* Register WiFi event handlers BEFORE calling esp_wifi_init (v0.2 fix) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_mode(WIFI_MODE_STA);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_inst_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &s_inst_got_ip));

    /* Try to connect with saved credentials, fall back to defaults */
    char saved_ssid[33] = "";
    char saved_password[65] = "";

    if (nvs_has_credentials()) {
        nvs_get_sta_ssid(saved_ssid, sizeof(saved_ssid));
        nvs_get_sta_password(saved_password, sizeof(saved_password));
        ESP_LOGI(TAG, "Loaded saved WiFi: '%s'", saved_ssid);
    }

    /* Fall back to hardcoded defaults if NVS has no credentials */
    if (saved_ssid[0] == '\0') {
        strncpy(saved_ssid, WIFI_SSID_DEFAULT, sizeof(saved_ssid) - 1);
        strncpy(saved_password, WIFI_PASSWORD_DEFAULT, sizeof(saved_password) - 1);
        ESP_LOGI(TAG, "No saved credentials, using default: '%s'", saved_ssid);
    }

    int sta_ok = -1;

    ESP_LOGI(TAG, "Attempting to connect to '%s'...", saved_ssid);
    sta_ok = wifi_init_sta(saved_ssid, saved_password);

    if (sta_ok == 0) {
        ESP_LOGI(TAG, "STA connected successfully");
        /* Keep event handlers registered so WiFi stack can handle
         * reconnection / disconnection events while running. */
        return 0;
    }

    /* STA failed — cleanup event handlers before switching to AP */
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_inst_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_inst_any_id);

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    /* STA failed — start AP fallback */
    ESP_LOGW(TAG, "STA failed, starting SoftAP 'mqtt-broker'...");

    esp_wifi_stop();
    s_wifi_mode = s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP;

    /* ESP32-S3 v0.2: must create AP netif BEFORE esp_wifi_set_mode */
    esp_netif_t *ap_nif = esp_netif_create_default_wifi_ap();
    apply_ap_ip(ap_nif);
    {
        char host[33] = "";
        portal_get_hostname(host, sizeof(host));
        if (ap_nif && host[0]) esp_netif_set_hostname(ap_nif, host);
    }
    esp_wifi_set_mode(s_wifi_mode);
    esp_wifi_start();

    ESP_LOGI(TAG, "SoftAP 'mqtt-broker' started (pw: mqtt1234), mode=%s",
             s_ap_enabled ? "AP+STA" : "AP-only");

    return 0;
}

int wifi_connect_sta_ex(const char *ssid, const char *password, int ap_enabled)
{
    s_ap_enabled = ap_enabled;
    return wifi_connect_sta();
}

void wifi_start_ap(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *ap_nif = esp_netif_create_default_wifi_ap();
    apply_ap_ip(ap_nif);

    s_wifi_mode = WIFI_MODE_AP;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_start();

    wifi_init_ap();
    ESP_LOGI(TAG, "AP started");
}

void wifi_start_apsta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_nif = esp_netif_create_default_wifi_ap();
    apply_ap_ip(ap_nif);

    s_wifi_mode = WIFI_MODE_APSTA;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_start();

    wifi_init_ap();
    ESP_LOGI(TAG, "AP+STA started");
}

void wifi_stop_ap(void)
{
    if (s_wifi_mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_AP);
        ESP_LOGI(TAG, "Switched from AP+STA to AP only");
    } else if (s_wifi_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "Stopped AP");
    }
    s_wifi_mode = WIFI_MODE_STA;
    esp_wifi_start();
}

int wifi_get_sta_connected(void)
{
    wifi_ap_record_t ap = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    return (err == ESP_OK);
}

void wifi_set_ap_mode(int enabled)
{
    s_ap_enabled = enabled;

    if (enabled && wifi_get_sta_connected()) {
        ESP_LOGI(TAG, "Enabling AP mode alongside STA");

        /* Create AP netif BEFORE changing mode (ESP32-S3 v0.2 requirement) */
        esp_netif_t *ap_nif = esp_netif_create_default_wifi_ap();
        apply_ap_ip(ap_nif);

        /* Load AP config from NVS */
        wifi_config_t ap_config;
        apply_ap_config(&ap_config);

        /* Switch to AP+STA — WiFi is already running, don't call esp_wifi_start() */
        s_wifi_mode = WIFI_MODE_APSTA;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

        ESP_LOGI(TAG, "AP '%s' started alongside STA", (char *)ap_config.ap.ssid);
    } else if (!enabled && wifi_get_sta_connected()) {
        ESP_LOGI(TAG, "Disabling AP mode");
        s_wifi_mode = WIFI_MODE_STA;
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
}

int wifi_get_ap_mode(void)
{
    return s_ap_enabled;
}

/* ---- NVS persistence functions (used by portal) ---- */

void portal_save_wifi(const char *ssid, const char *password, int ap_mode)
{
    nvs_save_sta_config(ssid, password, ap_mode ? 1 : 0);
    ESP_LOGI(TAG, "WiFi credentials saved: SSID='%s', ap_mode=%d", ssid, ap_mode);
}

void portal_load_wifi_state(void)
{
    s_ap_enabled = nvs_get_ap_mode();
    ESP_LOGI(TAG, "Loaded portal state: ap_enabled=%d", s_ap_enabled);
}

void portal_clear_wifi(void)
{
    nvs_clear_sta_config();
    ESP_LOGI(TAG, "WiFi credentials cleared from NVS");
}

int portal_reconnect_wifi(void)
{
    if (!nvs_has_credentials()) {
        ESP_LOGW(TAG, "No saved credentials to reconnect");
        return -1;
    }

    char ssid[33] = "";
    char password[65] = "";
    nvs_get_sta_ssid(ssid, sizeof(ssid));
    nvs_get_sta_password(password, sizeof(password));

    int ret = wifi_init_sta(ssid, password);
    if (ret == 0) {
        ESP_LOGI(TAG, "Reconnected to '%s'", ssid);
    } else {
        ESP_LOGW(TAG, "Reconnect failed for '%s'", ssid);
    }

    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;
    return ret;
}

int portal_get_ap_enabled(int *enabled)
{
    if (!enabled) return -1;
    *enabled = s_ap_enabled ? 1 : 0;
    return 0;
}

int portal_set_ap_enabled(int enabled)
{
    s_ap_enabled = enabled ? 1 : 0;
    return 0;
}

int portal_get_sta_status(int *sta_connected, int *ap_running,
                           char *ip_str, size_t ip_size,
                           char *ssid_str, size_t ssid_size)
{
    wifi_ap_record_t ap = {0};
    int connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    if (sta_connected) *sta_connected = connected ? 1 : 0;

    if (connected) {
        esp_netif_ip_info_t ip;
        esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
            if (ip_str && ip_size > 0) {
                snprintf(ip_str, ip_size, "%lu.%lu.%lu.%lu",
                    (unsigned long)(ip.ip.addr & 0xFF), (unsigned long)((ip.ip.addr >> 8) & 0xFF),
                    (unsigned long)((ip.ip.addr >> 16) & 0xFF), (unsigned long)((ip.ip.addr >> 24) & 0xFF));
            }
            if (ssid_str && ssid_size > 0) {
                snprintf(ssid_str, ssid_size, "%s", (char *)ap.ssid);
            }
        }
    }

    if (ap_running) *ap_running = (s_wifi_mode == WIFI_MODE_AP || s_wifi_mode == WIFI_MODE_APSTA) ? 1 : 0;

    return 0;
}

void wifi_get_ap_ip_str(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;

    /* Try to read from the live AP netif first */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(buf, buf_size, "%lu.%lu.%lu.%lu",
                     (unsigned long)(ip.ip.addr & 0xFF),
                     (unsigned long)((ip.ip.addr >> 8) & 0xFF),
                     (unsigned long)((ip.ip.addr >> 16) & 0xFF),
                     (unsigned long)((ip.ip.addr >> 24) & 0xFF));
            return;
        }
    }

    /* Fallback: load from NVS or Kconfig default */
    char mask[16];
    load_ap_ip(buf, buf_size, mask, sizeof(mask));
}
