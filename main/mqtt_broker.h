/*
 * MQTT Broker for ESP32-S3
 * Pure ESP-IDF implementation using lwIP BSD sockets.
 */

#ifndef MQTT_BROKER_H
#define MQTT_BROKER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- Tester (web UI MQTT topic tester) feature flag ----
 * When 1, the broker maintains a small registry of in-process "tester"
 * consumers (the web UI WebSocket clients) that receive every accepted
 * PUBLISH after normal fanout, and accepts publish requests from those
 * consumers via a thread-safe queue drained by broker_task. When 0, all
 * tester code is compiled out and the broker behaves exactly as before.
 */
#ifndef BROKER_TESTER_ENABLED
#define BROKER_TESTER_ENABLED 1
#endif

/* ---- Configuration ---- */
#define BROKER_MAX_CLIENTS          100
#define BROKER_MAX_SUBSCRIPTIONS    2048  /* total across all clients */
#define BROKER_RECV_BUF_SIZE_DEFAULT 16384 /* per-client receive buffer default */
#define BROKER_SEND_BUF_SIZE_DEFAULT 16384 /* shared send buffer default */
#define BROKER_PORT                 1883
#define BROKER_SELECT_TIMEOUT_MS    100
#define BROKER_KEEPALIVE_GRACE_SEC  10    /* extra seconds before disconnect */

/* ---- Outbound QoS 1 in-flight tracking ----
 * When a subscriber is granted QoS 1, every PUBLISH we send to them is
 * tracked in an in-flight slot until the matching PUBACK arrives. If the
 * PUBACK doesn't come within the retry timeout the message is resent with
 * DUP=1. After BROKER_INFLIGHT_RETRY_MAX retransmissions the broker gives
 * up and frees the slot (the subscriber will resync via reconnect if it
 * was a clean=false session — not yet implemented; phase 4).
 */
#define BROKER_INFLIGHT_PER_CLIENT_MAX  20
#define BROKER_INFLIGHT_TOTAL_BYTES_MAX (2u * 1024u * 1024u)  /* 2 MB global cap */
#define BROKER_INFLIGHT_RETRY_INITIAL_MS 15000
#define BROKER_INFLIGHT_RETRY_MAX_MS     60000  /* cap for exponential backoff */
#define BROKER_INFLIGHT_RETRY_MAX        5      /* abandon after N retries */

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
 * a web browser at the AP IP (default 192.168.25.1). */
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
    int      inflight;        /* outbound QoS 1 messages awaiting PUBACK */
    uint32_t published;       /* total PUBLISH messages accepted from this client */
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

#if BROKER_TESTER_ENABLED

/* ---- Tester API (consumed by portal_ws.c) ----
 *
 * Threading model:
 *   - All broker state (clients, subs, retained store) is owned by broker_task.
 *   - Tester consumers live in portal-side WS tasks.
 *   - The broker exposes ONE thread-safe FreeRTOS queue for publish requests
 *     from WS tasks -> broker_task (drained at the top of the select loop).
 *   - Each WS task creates its OWN FreeRTOS StreamBuffer and registers it
 *     with the broker. After a successful PUBLISH fanout, broker_task does
 *     a non-blocking xStreamBufferSend to each registered buffer. Full
 *     buffers drop messages (counted in tester stats); they never block
 *     the broker.
 */

/* Hard caps for tester. Keep small to bound RAM and risk. */
#define BROKER_TESTER_MAX_CONSUMERS    2
#define BROKER_TESTER_MAX_TOPIC_LEN    128
/* Bumped from 256 to 1024 in 0.6.0: the web tester needs to send/receive
 * real-world payloads (sensor JSON, config blobs). Each queued event adds
 * ~768 bytes; with BROKER_TESTER_MAX_CONSUMERS=2 and a small stream buffer
 * depth that's well under 50 KB total. */
#define BROKER_TESTER_MAX_PAYLOAD_LEN  1024

/* Wire format for messages handed from broker -> WS consumer via stream buffer.
 * Stored back-to-back, each preceded by its length (handled by StreamBuffer
 * trigger-level semantics). */
typedef struct {
    uint32_t seq;                 /* monotonically increasing per-broker */
    uint16_t topic_len;
    uint16_t payload_len;
    uint8_t  retain;              /* 0 or 1 */
    uint8_t  truncated;           /* payload was truncated to fit */
    char     topic[BROKER_TESTER_MAX_TOPIC_LEN + 1];
    uint8_t  payload[BROKER_TESTER_MAX_PAYLOAD_LEN];
} broker_tester_event_t;

/* Stats accessible to the portal for UI/debug. */
typedef struct {
    uint32_t consumers;
    uint32_t events_published;     /* total events handed to fanout */
    uint32_t events_dropped;       /* stream buffer was full */
    uint32_t publish_requests;     /* publishes from UI accepted */
    uint32_t publish_rejected;     /* malformed / over-limit */
} broker_tester_stats_t;

/* Initialize tester subsystem. Idempotent. Safe to call before broker_start().
 * Returns true on success. On failure, broker continues without tester. */
bool broker_tester_init(void);

/* Register a consumer's stream buffer. Returns slot index >=0 or -1 if full.
 * The caller (WS task) owns the StreamBufferHandle_t and must call
 * broker_tester_unregister() before deleting it. */
int broker_tester_register(void *stream_buffer_handle);

/* Unregister by slot index returned from broker_tester_register(). */
void broker_tester_unregister(int slot);

/* Number of currently-registered consumers (for /ws 503 gating). */
int broker_tester_consumer_count(void);

/* Submit a publish request from a WS task. Topic/payload are copied into the
 * queue item. Returns true if queued. Non-blocking; returns false if queue
 * is full or args invalid. The broker enforces wildcard/length rules. */
bool broker_tester_request_publish(const char *topic, size_t topic_len,
                                   const uint8_t *payload, size_t payload_len,
                                   bool retain);

void broker_tester_get_stats(broker_tester_stats_t *out);

#endif /* BROKER_TESTER_ENABLED */

#endif /* MQTT_BROKER_H */
