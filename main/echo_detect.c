/*
 * Echo Detection — Implementation
 *
 * Tracks per-topic publish history and detects echo loops:
 * when a topic receives >= N publishes within M seconds, it's flagged.
 */

#include "echo_detect.h"

#include "ntp.h"  /* ntp_is_synced() for detected_at timestamps */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "echo_detect";

/* ---- State ---- */

typedef struct {
    char     topic[MQTT_MAX_TOPIC_LEN + 1];
    uint16_t topic_len;
    uint16_t count;          /* publishes in current window */
    int64_t  window_start;   /* when the current window started (ms) */
    int64_t  detected_at;    /* first detection timestamp (ms), 0 if not detected */
} echo_entry_t;

static echo_entry_t s_entries[ECHO_MAX_TOPICS];
static int          s_count = 0;

static bool    s_enabled = true;
static uint16_t s_min_count = ECHO_MIN_COUNT_DEFAULT;
static uint16_t s_window_sec = ECHO_WINDOW_SEC_DEFAULT;

static portMUX_TYPE s_echo_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- Helpers ---- */

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int64_t ntp_now_ms(void)
{
    if (!ntp_is_synced()) return now_ms();
    return (int64_t)(ntp_now_us_corrected() / 1000);
}

static int find_entry(const char *topic, uint16_t topic_len)
{
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].topic_len == topic_len &&
            memcmp(s_entries[i].topic, topic, topic_len) == 0)
            return i;
    }
    return -1;
}

static int find_free_slot(void)
{
    if (s_count < ECHO_MAX_TOPICS) return s_count;
    /* Evict oldest entry with no detection */
    int oldest = -1;
    int64_t oldest_det = INT64_MAX;
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].detected_at == 0) {
            if (s_entries[i].window_start < oldest_det) {
                oldest_det = s_entries[i].window_start;
                oldest = i;
            }
        }
    }
    if (oldest >= 0) return oldest;
    /* All entries detected — evict oldest */
    oldest = 0;
    oldest_det = s_entries[0].detected_at;
    for (int i = 1; i < s_count; i++) {
        if (s_entries[i].detected_at < oldest_det) {
            oldest_det = s_entries[i].detected_at;
            oldest = i;
        }
    }
    return oldest;
}

/* ---- Public API ---- */

void echo_init(void)
{
    echo_load_config();
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    ESP_LOGI(TAG, "Echo detection initialized (enabled=%d, min=%u, window=%us)",
             s_enabled, s_min_count, s_window_sec);
}

bool echo_track(const char *topic, uint16_t topic_len)
{
    if (!s_enabled) return false;

    portENTER_CRITICAL(&s_echo_mux);

    int64_t now = now_ms();
    int64_t slot_ms = (int64_t)s_window_sec * 1000;

    int idx = find_entry(topic, topic_len);
    if (idx < 0) {
        /* New topic — find a slot and start fresh */
        idx = find_free_slot();
        if (idx >= s_count) s_count++;

        s_entries[idx].topic_len = topic_len;
        if (topic_len > MQTT_MAX_TOPIC_LEN) topic_len = MQTT_MAX_TOPIC_LEN;
        memcpy(s_entries[idx].topic, topic, topic_len);
        s_entries[idx].topic[topic_len] = '\0';
        s_entries[idx].count = 1;
        s_entries[idx].window_start = now;
        s_entries[idx].detected_at = 0;

        if (s_min_count == 1) {
            s_entries[idx].detected_at = ntp_now_ms();
        }
    } else {
        echo_entry_t *e = &s_entries[idx];

        if ((now - e->window_start) > slot_ms) {
            /* Window expired — start a new one */
            e->count = 1;
            e->window_start = now;
            /* Don't reset detected_at — once detected, stay detected */
        } else {
            e->count++;
        }

        if (e->detected_at == 0 && e->count >= s_min_count) {
            e->detected_at = ntp_now_ms();
        }
    }

    portEXIT_CRITICAL(&s_echo_mux);

    bool detected = (idx >= 0 && s_entries[idx].detected_at > 0);
    if (detected) {
        ESP_LOGI(TAG, "Echo detected: '%.*s' (%u publishes in %us window)",
                 topic_len, topic, s_entries[idx].count, s_window_sec);
    }
    return detected;
}

void echo_reset(void)
{
    portENTER_CRITICAL(&s_echo_mux);
    for (int i = 0; i < s_count; i++) {
        s_entries[i].detected_at = 0;
        s_entries[i].count = 0;
        s_entries[i].window_start = 0;
    }
    portEXIT_CRITICAL(&s_echo_mux);
    ESP_LOGI(TAG, "Echo detection state cleared");
}

void echo_get_detected(echo_detected_list_t *out)
{
    if (!out) return;

    out->count = 0;

    portENTER_CRITICAL(&s_echo_mux);
    for (int i = 0; i < s_count; i++) {
        echo_entry_t *e = &s_entries[i];
        if (e->detected_at > 0) {
            echo_detected_t *det = &out->entries[out->count];
            memcpy(det->topic, e->topic, e->topic_len);
            det->topic[e->topic_len] = '\0';
            det->topic[128] = '\0';
            det->count = e->count;
            det->detected_at = e->detected_at;
            out->count++;
        }
    }
    portEXIT_CRITICAL(&s_echo_mux);
}

bool echo_is_enabled(void)
{
    return s_enabled;
}

uint16_t echo_get_min_count(void)
{
    return s_min_count;
}

uint16_t echo_get_window_sec(void)
{
    return s_window_sec;
}

void echo_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open("mqtt_cfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t val8;
        if (nvs_get_u8(h, "echo_en", &val8) == ESP_OK) {
            s_enabled = (val8 != 0);
        }
        uint16_t val16;
        if (nvs_get_u16(h, "echo_count", &val16) == ESP_OK) {
            s_min_count = val16;
        }
       if (nvs_get_u16(h, "echo_window", &val16) == ESP_OK) {
            s_window_sec = val16;
        }
        nvs_close(h);
    }
}

void echo_save_config(void)
{
    nvs_handle_t h;
    if (nvs_open("mqtt_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "echo_en", s_enabled ? 1 : 0);
        nvs_set_u16(h, "echo_count", s_min_count);
        nvs_set_u16(h, "echo_window", s_window_sec);
        nvs_commit(h);
        nvs_close(h);
    }
}
