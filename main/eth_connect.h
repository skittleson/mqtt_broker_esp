/*
 * Ethernet (W5500 SPI) connection helper for ESP-IDF.
 *
 * Optional module — only compiled when CONFIG_MQTT_BROKER_ETHERNET is enabled.
 * Provides Ethernet connectivity via a W5500 SPI module, with DHCP client
 * to obtain an IP on the LAN side.
 */

#ifndef ETH_CONNECT_H
#define ETH_CONNECT_H

#include "sdkconfig.h"

#ifdef CONFIG_MQTT_BROKER_ETHERNET

#include "esp_err.h"
#include "esp_netif.h"

/**
 * Initialize W5500 SPI Ethernet.
 *
 * Sets up the SPI bus, W5500 MAC/PHY, network interface, and event handlers.
 * Blocks until an IP is acquired via DHCP (30s timeout).
 *
 * @return ESP_OK on success, ESP_FAIL on timeout or init error.
 */
esp_err_t eth_init(void);

/**
 * Get the Ethernet esp_netif handle.
 * Useful for enabling NAPT or querying IP info.
 *
 * @return esp_netif_t pointer, or NULL if not initialized.
 */
esp_netif_t *eth_get_netif(void);

/**
 * Check if Ethernet link is up and has an IP address.
 *
 * @return 1 if connected with IP, 0 otherwise.
 */
int eth_is_connected(void);

/**
 * Get the Ethernet IP address as a dotted string.
 *
 * @param buf       Output buffer for the IP string.
 * @param buf_size  Size of buf (at least 16 bytes recommended).
 * @return 0 on success, -1 if not connected or error.
 */
int eth_get_ip_str(char *buf, size_t buf_size);

/**
 * Enable NAPT on the WiFi AP interface.
 * Allows Ethernet LAN devices to reach WiFi AP clients.
 *
 * @return 0 on success, -1 on failure.
 */
int eth_napt_enable(void);

/**
 * Disable NAPT on the WiFi AP interface.
 * WiFi AP clients become isolated from the Ethernet LAN.
 *
 * @return 0 on success, -1 on failure.
 */
int eth_napt_disable(void);

/**
 * Check if NAPT is currently enabled.
 *
 * @return 1 if enabled, 0 if disabled.
 */
int eth_napt_is_enabled(void);

/**
 * Return 1 if the W5500 PHY currently reports link-up, 0 otherwise.
 * Useful for distinguishing "no cable / no link" from "link up but no DHCP".
 */
int eth_is_link_up(void);

/**
 * Return a short human-readable status string describing the current
 * Ethernet state machine stage. Always non-NULL.
 *
 * One of: "disabled", "init-fail", "started", "link-up", "got-ip",
 *        "link-down", "stopped".
 */
const char *eth_get_stage(void);

/**
 * Return a short human-readable last-error string, or "" if no error.
 */
const char *eth_get_last_error(void);

#endif /* CONFIG_MQTT_BROKER_ETHERNET */
#endif /* ETH_CONNECT_H */
