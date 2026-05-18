/*
 * Scheduled MQTT publishes ("Timers") — Tasmota-style.
 *
 * Each of TIMERS_SLOT_COUNT slots is a wall-clock schedule that, when its
 * conditions match the local time, publishes a configured MQTT message
 * through broker_publish_local(). DST is honoured automatically because
 * we resolve local time via newlib's tzset()/localtime_r() and the
 * POSIX TZ string stored in NVS ("ntp"/"tz").
 *
 * Storage: a single JSON blob in NVS "mqtt_cfg"/"timers" (compact form,
 * abbreviated keys) holding the schema version + per-slot config.
 *
 * Threading: timers_task is the only writer to last_fire_minute_utc and
 * the only caller of the fire path. The HTTP handlers touch s_slots
 * under s_lock (FreeRTOS mutex). timers_task takes the same lock for
 * its 1Hz sweep.
 */

#ifndef TIMERS_H
#define TIMERS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define TIMERS_SLOT_COUNT       16
#define TIMERS_TOPIC_MAX        128
#define TIMERS_PAYLOAD_MAX      256
#define TIMERS_LABEL_MAX        24

/* Bit positions in `days` mask. Sunday is bit 0 (matches POSIX tm_wday). */
#define TIMERS_DAY_SUN          (1u << 0)
#define TIMERS_DAY_MON          (1u << 1)
#define TIMERS_DAY_TUE          (1u << 2)
#define TIMERS_DAY_WED          (1u << 3)
#define TIMERS_DAY_THU          (1u << 4)
#define TIMERS_DAY_FRI          (1u << 5)
#define TIMERS_DAY_SAT          (1u << 6)
#define TIMERS_DAYS_ALL         0x7F
#define TIMERS_DAYS_WEEKDAYS    (TIMERS_DAY_MON | TIMERS_DAY_TUE | \
                                 TIMERS_DAY_WED | TIMERS_DAY_THU | \
                                 TIMERS_DAY_FRI)
#define TIMERS_DAYS_WEEKENDS    (TIMERS_DAY_SAT | TIMERS_DAY_SUN)

typedef struct {
    bool     arm;            /* schedule is active */
    bool     repeat;         /* false = fire once then disarm */
    bool     retain;
    uint8_t  qos;            /* 0 or 1 */
    uint8_t  days;           /* bitmask, bit 0=Sun .. bit 6=Sat */
    uint8_t  window;         /* 0..15 min of random jitter */
    uint16_t minute_of_day;  /* 0..1439 (local time, HH*60+MM) */
    char     topic[TIMERS_TOPIC_MAX + 1];
    char     payload[TIMERS_PAYLOAD_MAX + 1];
    uint16_t payload_len;
    char     label[TIMERS_LABEL_MAX + 1];
} timer_slot_t;

/* Master enable. When false, no slot fires regardless of per-slot `arm`. */
bool timers_master_enabled(void);
void timers_set_master_enabled(bool enabled);

/* Load schedules from NVS and start the 1Hz scheduler task. Safe to call
 * after ntp_init() so the TZ env var is already in place. */
esp_err_t timers_init(void);

/* Snapshot a slot (1-based index 1..TIMERS_SLOT_COUNT). Returns false on
 * out-of-range. The output is a deep copy; safe to read after return. */
bool timers_get(int slot_1based, timer_slot_t *out);

/* Replace a slot wholesale. Validates fields, persists to NVS, and clears
 * the slot's last-fire cache so a backward time edit doesn't replay.
 * Returns ESP_OK on success, ESP_ERR_INVALID_ARG with `err_out` populated
 * on validation failure (err_out may be NULL). */
esp_err_t timers_set(int slot_1based, const timer_slot_t *in,
                     char *err_out, size_t err_out_size);

/* Wipe a slot to defaults (arm=0, all fields cleared). Persists. */
esp_err_t timers_clear(int slot_1based);

/* Compute the next UTC unix time at which the given slot would fire.
 * Returns 0 if the slot is disarmed / never fires / clock unsynced.
 * Looks up to 8 days ahead; returns 0 if no match in that window. */
int64_t timers_next_fire_unix(int slot_1based);

/* Fire a slot right now, regardless of schedule. Used by the
 * "test fire" button. Returns ESP_OK on enqueue. */
esp_err_t timers_fire_now(int slot_1based);

/* Counter of fires that were dropped because the broker publish queue
 * was full (for the /timers debug line). */
uint32_t timers_dropped_count(void);

/* Parse / format the 7-char day mask used in the JSON wire format.
 * "SMTWTFS" \u2192 all on; "-MTWTF-" \u2192 weekdays; "-" = off. Returns false on
 * malformed input (wrong length / not 7 chars). */
bool timers_days_from_string(const char *s, uint8_t *out_mask);
void timers_days_to_string(uint8_t mask, char out[8]);

/* Validate fields without persisting. Used by the JSON PUT handler.
 * Returns ESP_OK if valid; on failure writes a human-readable message to
 * err_out (≤ err_out_size bytes). */
esp_err_t timers_validate(const timer_slot_t *in,
                          char *err_out, size_t err_out_size);

#endif /* TIMERS_H */
