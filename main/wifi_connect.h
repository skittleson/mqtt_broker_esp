/*
 * WiFi STA/AP connection helper for ESP-IDF.
 *
 * Supports:
 *   - Loading/saving WiFi credentials from NVS
 *   - STA mode, AP mode, and AP+STA mode
 *   - Automatic AP fallback when STA connection fails
 */

#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include <stdint.h>

/* Default WiFi credentials — used as fallback if no NVS credentials.
 * Leave empty to boot into AP mode on first use (recommended).
 * Users configure WiFi via the captive portal at 192.168.4.1. */
#define WIFI_SSID_DEFAULT       ""
#define WIFI_PASSWORD_DEFAULT   ""

/**
 * Initialize WiFi in STA mode and connect to the configured AP.
 * Loads credentials from NVS if available, uses defaults as fallback.
 * Falls back to SoftAP mode on failure.
 * Blocks until connected or failed — 60s timeout.
 * Returns 0 on success (STA connected or AP started), -1 on failure.
 */
int wifi_connect_sta(void);

/**
 * Connect as STA with explicit credentials, optional AP mode alongside.
 * @param ssid       WiFi network name
 * @param password   WiFi password
 * @param ap_enabled 1 = enable AP mode alongside STA, 0 = STA only
 * @return 0 on success, -1 on failure
 */
int wifi_connect_sta_ex(const char *ssid, const char *password, int ap_enabled);

/**
 * Start WiFi in AP-only mode (no STA).
 * Use this when you want the device to act as an access point.
 */
void wifi_start_ap(void);

/**
 * Start WiFi in AP+STA mode (both AP and STA active).
 * Use this when you want both a local portal and cloud connectivity.
 */
void wifi_start_apsta(void);

/**
 * Stop AP mode, keep STA running.
 * No-op if not in AP or AP+STA mode.
 */
void wifi_stop_ap(void);

/**
 * Check if STA is currently connected to an AP.
 * @return 1 if connected, 0 if not
 */
int wifi_get_sta_connected(void);

/**
 * Enable or disable AP mode (when STA is connected).
 * @param enabled 1 = enable AP, 0 = disable AP
 */
void wifi_set_ap_mode(int enabled);

/**
 * Get current AP mode state.
 * @return 1 if AP enabled, 0 if disabled
 */
int wifi_get_ap_mode(void);

#endif /* WIFI_CONNECT_H */
