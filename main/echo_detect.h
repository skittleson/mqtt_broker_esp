/*
 * Echo Detection
 *
 * Detects per-topic echo loops: when a topic receives >= N publishes
 * within a sliding M-second window, the broker flags it as a potential
 * echo loop.
 *
 * Triggered by handle_publish_internal() after fanout in broker_task.
 * State is PSRAM-backed, in-memory only — no NVS writes on detection.
 */

#ifndef ECHO_DETECT_H
#define ECHO_DETECT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "mqtt_broker.h"
#include "mqtt_parser.h"  /* MQTT_MAX_TOPIC_LEN */

/* ---- Configuration ---- */

#define ECHO_MAX_TOPICS     32          /* max tracked topics */
#define ECHO_WINDOW_SEC_DEFAULT  60     /* default detection window */
#define ECHO_MIN_COUNT_DEFAULT  3      /* default min publishes to trigger */

/* ---- Public API (called from broker_task only) ---- */

/* Initialize echo detection state. Must be called before broker_start(). */
void echo_init(void);

/* Track a PUBLISH to the given topic. Called from handle_publish_internal().
 * Returns true if the topic was just detected as an echo loop. */
bool echo_track(const char *topic, uint16_t topic_len);

/* Reset all detection state. Used by /api/echo-reset. */
void echo_reset(void);

/* ---- API (consumed by portal.c) ---- */

typedef struct {
    char     topic[129];             /* max topic len */
    uint16_t count;
    int64_t  detected_at;
} echo_detected_t;

typedef struct {
    echo_detected_t entries[ECHO_MAX_TOPICS];
    int count;
} echo_detected_list_t;

/* Get a snapshot of detected echo topics (thread-safe via critical section). */
void echo_get_detected(echo_detected_list_t *out);

/* ---- Config (read from NVS at init) ---- */

bool   echo_is_enabled(void);
uint16_t echo_get_min_count(void);
uint16_t echo_get_window_sec(void);

/* Load config from NVS. */
void echo_load_config(void);

/* Save config to NVS. */
void echo_save_config(void);

#endif /* ECHO_DETECT_H */
