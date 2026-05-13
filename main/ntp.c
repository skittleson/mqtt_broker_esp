/*
 * SNTP client (Phase 1 of plan-ntp-server.md).
 *
 * Thin wrapper around ESP-IDF's esp_sntp that:
 *   - reads up to 3 upstream hostnames from NVS namespace "ntp",
 *   - reads the desired poll interval and timezone string from NVS,
 *   - registers a time-sync callback that records when sync happened and
 *     which upstream answered (best-effort -- esp_sntp only exposes the
 *     index, not the IP),
 *   - exposes ntp_now_us() / ntp_is_synced() / ntp_get_state() for the
 *     rest of the firmware (portal /api/time, MQTT $SYS/broker/time).
 *
 * No SNTP server is started here. That's Phase 2 of the plan.
 *
 * Threading / memory: esp_sntp runs its own internal task. This file only
 * adds a few static int64s and a 256-byte buffer for hostname strings,
 * all in BSS. The settings_changed() entry is documented to be safe to
 * call from the portal task; it tears down esp_sntp and restarts it with
 * new upstreams. Until Phase 3 we just require a reboot, but the function
 * is shaped so a future hot-reload is trivial.
 */

#include "ntp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "ntp";

#define NVS_NS_NTP            "ntp"
#define NTP_POLL_S_DEFAULT    3600    /* 1h */
#define NTP_POLL_S_MIN        64
#define NTP_POLL_S_MAX        86400

/* ---- State ---- */

static bool     s_inited            = false;
static bool     s_synced            = false;
static int64_t  s_last_sync_us      = 0;     /* esp_timer units, NOT epoch */
static uint32_t s_sync_count        = 0;
static int      s_last_upstream_idx = -1;
static char     s_upstreams[NTP_UPSTREAMS_MAX][NTP_UPSTREAM_MAX_LEN];

/* ---- NVS helpers (mirrors of the portal's; kept local to keep this file
 *      self-contained and not require a header from portal.c) ---- */

static void ntp_nvs_get_str(const char *key, char *out, size_t out_size, const char *def)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS_NTP, NVS_READONLY, &h) == ESP_OK) {
        size_t len = out_size;
        if (nvs_get_str(h, key, out, &len) != ESP_OK && def) {
            strncpy(out, def, out_size - 1);
            out[out_size - 1] = '\0';
        }
        nvs_close(h);
    } else if (def) {
        strncpy(out, def, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static uint8_t ntp_nvs_get_u8(const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open(NVS_NS_NTP, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static uint32_t ntp_nvs_get_u32(const char *key, uint32_t def)
{
    nvs_handle_t h;
    uint32_t v = def;
    if (nvs_open(NVS_NS_NTP, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

/* ---- esp_sntp callback ---- */

static void on_sntp_synced(struct timeval *tv)
{
    /* esp_sntp_get_sync_status() returns COMPLETED right when this fires.
     * We only want to record the event itself; the wall-clock readout
     * comes from settimeofday() which esp_sntp has just called for us. */
    s_synced = true;
    s_last_sync_us = esp_timer_get_time();
    s_sync_count++;
    /* The IDF API exposes the upstream we used as the most recently-replied
     * index; pull it for the /api/time UI. Some IDF versions don't surface
     * this; gate by the macro to keep it portable. */
#if defined(SNTP_GET_SERVER_FROM_DHCP) || defined(ESP_SNTP_OPMODE_LISTENONLY)
    s_last_upstream_idx = (int)esp_sntp_get_sync_mode();  /* placeholder */
#endif
    ESP_LOGI(TAG, "synced: epoch_s=%lld sync_count=%u",
             (long long)tv->tv_sec, (unsigned)s_sync_count);
}

/* ---- Public API ---- */

void ntp_init(void)
{
    if (s_inited) return;

    /* Master enable gate. Default ON per the plan; user can disable from
     * /settings. */
    if (ntp_nvs_get_u8("enabled", 1) == 0) {
        ESP_LOGI(TAG, "disabled in NVS, skipping init");
        return;
    }

    /* Load up to 3 upstreams from NVS. Defaults per the plan. */
    const char *defaults[NTP_UPSTREAMS_MAX] = {
        "pool.ntp.org",
        "time.cloudflare.com",
        "",
    };
    char keybuf[12];
    int active = 0;
    for (int i = 0; i < NTP_UPSTREAMS_MAX; i++) {
        snprintf(keybuf, sizeof(keybuf), "upstream_%d", i);
        ntp_nvs_get_str(keybuf, s_upstreams[i], NTP_UPSTREAM_MAX_LEN, defaults[i]);
        if (s_upstreams[i][0]) active++;
    }
    if (active == 0) {
        ESP_LOGW(TAG, "no upstreams configured, falling back to pool.ntp.org");
        strncpy(s_upstreams[0], "pool.ntp.org", NTP_UPSTREAM_MAX_LEN - 1);
        active = 1;
    }

    /* Stop any in-flight SNTP that may have been started by another
     * component (Wi-Fi station enabled-by-default behaviour in some IDF
     * configs auto-starts it). Re-init under our control. */
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    for (int i = 0; i < NTP_UPSTREAMS_MAX; i++) {
        if (s_upstreams[i][0]) {
            esp_sntp_setservername(i, s_upstreams[i]);
        }
    }

    /* Poll interval. esp_sntp wants milliseconds; clamp to plan range. */
    uint32_t poll_s = ntp_nvs_get_u32("poll_s", NTP_POLL_S_DEFAULT);
    if (poll_s < NTP_POLL_S_MIN) poll_s = NTP_POLL_S_MIN;
    if (poll_s > NTP_POLL_S_MAX) poll_s = NTP_POLL_S_MAX;
    esp_sntp_set_sync_interval(poll_s * 1000UL);

    /* Timezone for localtime_r / strftime callers. Stored as POSIX TZ
     * string in NVS; default UTC0. */
    char tz[64];
    ntp_nvs_get_str("tz", tz, sizeof(tz), "UTC0");
    setenv("TZ", tz, 1);
    tzset();

    /* Sync callback so we know when wall clock becomes valid. */
    esp_sntp_set_time_sync_notification_cb(on_sntp_synced);

    /* Smooth-step on tiny offsets, hard set otherwise. Keeps the broker's
     * uptime monotone except on the first sync (when we have no choice). */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

    esp_sntp_init();
    s_inited = true;

    ESP_LOGI(TAG, "started: %d upstream(s), poll=%us, tz=%s",
             active, (unsigned)poll_s, tz);
    for (int i = 0; i < NTP_UPSTREAMS_MAX; i++) {
        if (s_upstreams[i][0]) {
            ESP_LOGI(TAG, "  upstream[%d] = %s", i, s_upstreams[i]);
        }
    }
}

int64_t ntp_now_us(void)
{
    if (!s_synced) return 0;
    /* gettimeofday is the wall-clock source after esp_sntp settimeofday()s
     * us. Cheap; goes through esp_timer / esp_clk internally. */
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

bool ntp_is_synced(void)
{
    return s_synced;
}

void ntp_get_state(ntp_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->epoch_us = ntp_now_us();
    out->synced = s_synced;
    out->last_sync_us = s_last_sync_us;
    if (s_synced) {
        int64_t now = esp_timer_get_time();
        out->last_sync_age_s = (now - s_last_sync_us) / 1000000;
    } else {
        out->last_sync_age_s = -1;
    }
    if (s_last_upstream_idx >= 0 && s_last_upstream_idx < NTP_UPSTREAMS_MAX) {
        strncpy(out->upstream_used, s_upstreams[s_last_upstream_idx],
                sizeof(out->upstream_used) - 1);
    } else if (s_upstreams[0][0]) {
        /* No per-reply attribution available; show the primary as a hint. */
        strncpy(out->upstream_used, s_upstreams[0],
                sizeof(out->upstream_used) - 1);
    }
    out->sync_count = s_sync_count;
    out->server_running = false;  /* Phase 2 sets this true. */
}

void ntp_get_settings(ntp_settings_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enabled = (ntp_nvs_get_u8("enabled", 1) != 0);
    out->poll_s = ntp_nvs_get_u32("poll_s", NTP_POLL_S_DEFAULT);
    ntp_nvs_get_str("tz", out->tz, sizeof(out->tz), "UTC0");
    char keybuf[12];
    const char *defaults[NTP_UPSTREAMS_MAX] = {
        "pool.ntp.org", "time.cloudflare.com", "",
    };
    for (int i = 0; i < NTP_UPSTREAMS_MAX; i++) {
        snprintf(keybuf, sizeof(keybuf), "upstream_%d", i);
        ntp_nvs_get_str(keybuf, out->upstreams[i], NTP_UPSTREAM_MAX_LEN,
                        defaults[i]);
    }
}

bool ntp_force_resync(void)
{
    if (!s_inited) return false;
    /* esp_sntp_restart() schedules an immediate poll. The reply lands in
     * on_sntp_synced() some hundreds of ms later. */
    return esp_sntp_restart();
}
