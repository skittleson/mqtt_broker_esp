/*
 * MQTT Broker for ESP32-S3
 * Pure ESP-IDF implementation using lwIP BSD sockets.
 */

#ifndef MQTT_BROKER_H
#define MQTT_BROKER_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Configuration ---- */
#define BROKER_MAX_CLIENTS          100
#define BROKER_MAX_SUBSCRIPTIONS    2048  /* total across all clients */
#define BROKER_RECV_BUF_SIZE_DEFAULT 16384 /* per-client receive buffer default */
#define BROKER_SEND_BUF_SIZE_DEFAULT 16384 /* shared send buffer default */
#define BROKER_PORT                 1883
#define BROKER_SELECT_TIMEOUT_MS    100
#define BROKER_KEEPALIVE_GRACE_SEC  10    /* extra seconds before disconnect */

/* ---- Retained message configuration ---- */
#define BROKER_RETAIN_TTL_SEC_DEFAULT  (7 * 24 * 3600)  /* 1 week default */
#define BROKER_RETAIN_MEM_PCT       80                /* max % of PSRAM for retained msgs */
#define BROKER_RETAIN_MAX_MSG_SIZE  (64 * 1024)       /* reject single msgs larger than 64KB */

/* ---- Authentication configuration ---- */
/* Authentication is configured at runtime via the web portal.
 * Credentials are stored in NVS. When both username and password
 * are set, auth is enabled and clients must provide matching
 * credentials in CONNECT (CONNACK 0x04 on bad creds).
 * When either is empty, auth is disabled (open broker). */

#define MQTT_BROKER_AUTH_USERNAME_MAX_LEN  MQTT_MAX_TOPIC_LEN
#define MQTT_BROKER_AUTH_PASSWORD_MAX_LEN  MQTT_MAX_TOPIC_LEN

/* ---- Captive Portal AP mode ---- */
/* When enabled, the captive portal runs even if the device connects via
 * Ethernet (W5500 SPI). The portal provides WiFi configuration via
 * a web browser at the AP IP (192.168.4.1). */
/* #define MQTT_BROKER_ENABLE_PORTAL  1 */

/**
 * Start the MQTT broker.
 * Creates a FreeRTOS task that runs the select() loop.
 * Call this after WiFi is connected and has an IP.
 */
void broker_start(void);

/**
 * Get broker runtime statistics (thread-safe snapshot).
 */
typedef struct {
    int      connected_clients;
    int      active_subscriptions;
    uint32_t retained_count;
    uint32_t retained_bytes;
    uint32_t free_heap;
    int64_t  uptime_ms;
    bool     retain_enabled;
    int32_t  retain_ttl_sec;
    uint16_t buf_size;
} broker_stats_t;

void broker_get_stats(broker_stats_t *stats);

/**
 * Per-client info snapshot (for portal / API).
 */
typedef struct {
    char     client_id[64];   /* MQTT client identifier */
    char     ip[16];          /* IP address string */
    int      subscriptions;   /* number of active subscriptions */
    int64_t  connected_ms;    /* how long connected (milliseconds) */
    int64_t  last_active_ms;  /* ms since last activity */
    uint16_t keep_alive;      /* negotiated keep-alive (seconds) */
} broker_client_info_t;

/**
 * Get list of connected MQTT clients (thread-safe snapshot).
 * @param out        output array (caller-allocated)
 * @param max_out    max entries in output array
 * @return           number of connected clients written to out
 */
int broker_get_clients(broker_client_info_t *out, int max_out);

#endif /* MQTT_BROKER_H */
