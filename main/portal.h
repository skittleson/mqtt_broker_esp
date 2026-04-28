/*
 * Captive Portal — WiFi configuration via web browser in AP mode
 */

#ifndef PORTAL_H
#define PORTAL_H

#include <stddef.h>

/**
 * Start the captive portal (HTTP on port 80, DNS hijack on port 53).
 * Should only be called when the device is in AP mode.
 */
void portal_start(void);

/**
 * Stop the captive portal.
 */
void portal_stop(void);

/**
 * Save WiFi credentials.
 * @param ssid       WiFi network name (max 32 bytes)
 * @param password   WiFi password (max 64 bytes)
 * @param ap_mode    1 = also enable AP mode, 0 = STA only
 */
void portal_save_wifi(const char *ssid, const char *password, int ap_mode);

/**
 * Load saved WiFi state into portal globals.
 */
void portal_load_wifi_state(void);

/**
 * Clear saved WiFi credentials from NVS.
 */
void portal_clear_wifi(void);

/**
 * Reconnect to saved WiFi credentials (if any exist).
 * @return 0 on success, -1 on failure
 */
int portal_reconnect_wifi(void);

/**
 * Get current portal AP enabled state.
 * @param enabled  output: 0 = AP disabled, 1 = AP enabled
 * @return 0 on success
 */
int portal_get_ap_enabled(int *enabled);

/**
 * Set portal AP enabled state (for toggle).
 * @param enabled  0 = disable, 1 = enable
 * @return 0 on success
 */
int portal_set_ap_enabled(int enabled);

/**
 * Set the SSID displayed in the portal (called after saving credentials).
 * @param ssid  WiFi network name
 */
void portal_set_portal_ssid(const char *ssid);

/**
 * Get current WiFi/portal status.
 * @param sta_connected  output: 1 if STA connected, 0 if not
 * @param ap_running     output: 1 if AP is running, 0 if not
 * @param ip_str         output buffer for IP address string
 * @param ip_size        size of ip_str buffer
 * @param ssid_str       output buffer for SSID string
 * @param ssid_size      size of ssid_str buffer
 * @return 0 on success
 */
int portal_get_sta_status(int *sta_connected, int *ap_running,
                           char *ip_str, size_t ip_size,
                           char *ssid_str, size_t ssid_size);

#endif /* PORTAL_H */
