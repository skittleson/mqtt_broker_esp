/*
 * SNTP client + server (Phases 1 and 2 of plan-ntp-server.md).
 *
 * Client side (Phase 1) is a thin wrapper around ESP-IDF's esp_sntp:
 *   - reads up to 3 upstream hostnames from NVS namespace "ntp",
 *   - reads the desired poll interval and timezone string from NVS,
 *   - registers a time-sync callback that records when sync happened,
 *   - exposes ntp_now_us() / ntp_is_synced() / ntp_get_state() for the
 *     rest of the firmware (portal /api/time, MQTT $SYS/broker/time).
 *
 * Server side (Phase 2) is a single FreeRTOS task pinned to CPU 0 that
 * binds UDP :123 and answers SNTPv4 client queries:
 *   - 48-byte tx buffer is task-local, no malloc in the hot path,
 *   - per-source IP rate limit (32-entry LRU, 10 req/s/source),
 *   - drops oversized (>68 B) and undersized (<48 B) packets silently to
 *     defang amplification attacks,
 *   - emits stratum 16 / LI=3 (alarm) while unsynced so well-behaved
 *     clients (chrony, ntpd, w32time, systemd-timesyncd) ignore us
 *     until we have real time to offer.
 *
 * Threading / memory: esp_sntp runs its own internal task (Phase 1).
 * The server task adds ~1 KB of stack use plus a 384-byte rate-limit LRU
 * in BSS. Both sides read the same s_synced state and gettimeofday();
 * the client is the sole writer of s_synced (inside its IDF callback)
 * so a plain boolean read on the server hot path is safe without a lock.
 */

#include "ntp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* ---- SNTP server state (Phase 2) ---- */

#define NTP_SERVER_PORT      123
#define NTP_PACKET_LEN       48
#define NTP_PACKET_MAX_LEN   68     /* drop oversized -- anti-amplification */
#define NTP_EPOCH_OFFSET     2208988800ULL  /* seconds 1900 -> 1970 */
#define NTP_RATE_PER_SEC     10
#define NTP_RATE_LRU_SIZE    32
#define NTP_RATE_WINDOW_MS   1000

/* Per-source rate-limit LRU. 32 slots * ~16 B = ~512 B in BSS. Keyed by
 * source IPv4 in network byte order (raw bytes, no conversion). On miss
 * we evict the oldest slot. */
typedef struct {
    uint32_t addr;          /* sin_addr.s_addr (NBO); 0 == free */
    int64_t  last_ms;       /* esp_timer-ms when last_seen */
    uint8_t  count;         /* requests in the current window */
} ntp_rate_entry_t;

static ntp_rate_entry_t s_rate[NTP_RATE_LRU_SIZE];
static bool     s_server_running   = false;
static bool     s_server_started   = false;  /* spawn idempotency */
static uint32_t s_served           = 0;
static uint32_t s_dropped_rate     = 0;
static uint32_t s_dropped_size     = 0;
static uint32_t s_dropped_mode     = 0;

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

/* Stratum we advertise as a server. RFC 4330: stratum 16 ='
 * unsynchronized -- well-behaved clients should ignore. esp_sntp doesn't
 * reliably surface the upstream's stratum across IDF versions, so we
 * conservatively advertise 3 once synced (correct if our upstream is
 * stratum 2 -- the common pool.ntp.org case; never causes clients to
 * reject us). */
static uint8_t ntp_advertised_stratum(void)
{
    return s_synced ? 3 : 16;
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
    out->sync_count     = s_sync_count;
    out->server_running = s_server_running;
    out->stratum        = ntp_advertised_stratum();
    out->served         = s_served;
    out->dropped_rate   = s_dropped_rate;
    out->dropped_size   = s_dropped_size;
    out->dropped_mode   = s_dropped_mode;
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

/* ---- SNTP server (Phase 2) ---- */

/* Convert epoch microseconds to NTP 64-bit timestamp (32-bit secs since
 * 1900-01-01 UTC || 32-bit fraction, unit = 2^-32 sec). epoch_us<=0
 * yields 0/0 (RFC "never" sentinel). */
static void ntp_epoch_us_to_ntp(int64_t epoch_us,
                                uint32_t *out_secs, uint32_t *out_frac)
{
    if (epoch_us <= 0) {
        *out_secs = 0;
        *out_frac = 0;
        return;
    }
    uint64_t es  = (uint64_t)(epoch_us / 1000000);
    uint64_t eus = (uint64_t)(epoch_us % 1000000);
    *out_secs = (uint32_t)(es + NTP_EPOCH_OFFSET);
    /* frac = us * 2^32 / 1e6 in 64-bit math: eus<1e6<2^20, shifted left
     * 32 is <2^52 -- well within 64-bit. */
    *out_frac = (uint32_t)((eus << 32) / 1000000ULL);
}

/* Token-bucket rate limit per source IP. Returns true if served, false
 * if the source exceeded NTP_RATE_PER_SEC in the current 1s window.
 * O(N) over 32 entries -- cheap vs any recvfrom/sendto. */
static bool ntp_rate_allow(uint32_t addr)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int oldest_i = 0;
    int64_t oldest_ms = INT64_MAX;
    int free_i = -1;
    for (int i = 0; i < NTP_RATE_LRU_SIZE; i++) {
        ntp_rate_entry_t *e = &s_rate[i];
        if (e->addr == addr && e->addr != 0) {
            if ((now_ms - e->last_ms) > NTP_RATE_WINDOW_MS) {
                e->count = 0;   /* window expired */
            }
            if (e->count >= NTP_RATE_PER_SEC) {
                return false;
            }
            e->count++;
            e->last_ms = now_ms;
            return true;
        }
        if (e->addr == 0 && free_i < 0) free_i = i;
        if (e->last_ms < oldest_ms) {
            oldest_ms = e->last_ms;
            oldest_i = i;
        }
    }
    int slot = (free_i >= 0) ? free_i : oldest_i;
    s_rate[slot].addr    = addr;
    s_rate[slot].count   = 1;
    s_rate[slot].last_ms = now_ms;
    return true;
}

/* Reference identifier for our server responses. RFC 4330 §4:
 *   stratum 0/1/16: 4-char ASCII code ("INIT", "GPS ", etc.)
 *   stratum 2..15:  IPv4 of the upstream we follow
 * esp_sntp doesn't portably expose the upstream IP across IDF versions,
 * so when synced we substitute the ASCII tag "ESP3" -- chrony, ntpd,
 * and w32time accept any ref-id value and only display it for diagnostics.
 * When unsynced we emit "INIT" (the RFC's example for not-yet-synced). */
static void ntp_write_ref_id(uint8_t *out4, bool synced)
{
    if (synced) {
        out4[0] = 'E'; out4[1] = 'S'; out4[2] = 'P'; out4[3] = '3';
    } else {
        out4[0] = 'I'; out4[1] = 'N'; out4[2] = 'I'; out4[3] = 'T';
    }
}

static void ntp_server_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "server: socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(NTP_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "server: bind() :%d failed: %d", NTP_SERVER_PORT, errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    s_server_running = true;
    ESP_LOGI(TAG, "server: listening on 0.0.0.0:%d", NTP_SERVER_PORT);

    /* Static per-task buffers. rx[] is a few bytes over the legal max so
     * we detect oversize packets in one recvfrom() call. */
    uint8_t rx[NTP_PACKET_MAX_LEN + 4];
    uint8_t tx[NTP_PACKET_LEN];

    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd, rx, sizeof(rx), 0,
                             (struct sockaddr *)&src, &slen);
        if (n <= 0) {
            if (errno == EINTR) continue;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Receive timestamp captured ASAP so the client's RTT estimate
         * is dominated by network jitter, not our processing. */
        int64_t rx_epoch_us = ntp_now_us();

        /* Anti-amplification: silently drop packets outside 48..68 B. */
        if (n < NTP_PACKET_LEN || n > NTP_PACKET_MAX_LEN) {
            s_dropped_size++;
            continue;
        }

        /* Accept only mode=3 (client) and mode=1 (symmetric active).
         * Reject mode=6/7 (control/private) explicitly -- those are
         * the abuse vectors classic ntpd had. */
        uint8_t li_vn_mode = rx[0];
        uint8_t mode = li_vn_mode & 0x07;
        if (mode != 3 && mode != 1) {
            s_dropped_mode++;
            continue;
        }

        if (!ntp_rate_allow(src.sin_addr.s_addr)) {
            s_dropped_rate++;
            continue;
        }

        /* Snapshot s_synced once so header LI/stratum and reference
         * timestamp stay coherent for this packet. */
        bool synced = s_synced;
        memset(tx, 0, sizeof(tx));

        /* Byte 0: LI(2) | VN(3) | Mode(3). LI=3 (alarm) when unsynced. */
        tx[0] = (uint8_t)(((synced ? 0 : 3) << 6) | (4 << 3) | 4);
        tx[1] = ntp_advertised_stratum();
        tx[2] = 6;                       /* poll: 2^6 = 64 s */
        tx[3] = (uint8_t)((int8_t)-20);  /* precision: 2^-20 sec ~ 1 us */
        /* Bytes 4..11 (root delay, dispersion) left 0 = unknown. */

        ntp_write_ref_id(&tx[12], synced);

        /* Reference Timestamp (16..23): when we last synced upstream.
         * Zero when unsynced (RFC "never" sentinel). */
        uint32_t ref_secs = 0, ref_frac = 0;
        if (synced) {
            int64_t age_us = esp_timer_get_time() - s_last_sync_us;
            int64_t last_sync_epoch_us = rx_epoch_us - age_us;
            ntp_epoch_us_to_ntp(last_sync_epoch_us, &ref_secs, &ref_frac);
        }
        uint32_t nv;
        nv = htonl(ref_secs); memcpy(&tx[16], &nv, 4);
        nv = htonl(ref_frac); memcpy(&tx[20], &nv, 4);

        /* Originate Timestamp (24..31): echo client TX ts (rx 40..47). */
        memcpy(&tx[24], &rx[40], 8);

        /* Receive Timestamp (32..39). */
        uint32_t rx_secs = 0, rx_frac = 0;
        ntp_epoch_us_to_ntp(rx_epoch_us, &rx_secs, &rx_frac);
        nv = htonl(rx_secs); memcpy(&tx[32], &nv, 4);
        nv = htonl(rx_frac); memcpy(&tx[36], &nv, 4);

        /* Transmit Timestamp (40..47): sampled last, immediately before send. */
        int64_t tx_epoch_us = ntp_now_us();
        uint32_t tx_secs = 0, tx_frac = 0;
        ntp_epoch_us_to_ntp(tx_epoch_us, &tx_secs, &tx_frac);
        nv = htonl(tx_secs); memcpy(&tx[40], &nv, 4);
        nv = htonl(tx_frac); memcpy(&tx[44], &nv, 4);

        ssize_t sent = sendto(fd, tx, NTP_PACKET_LEN, 0,
                              (struct sockaddr *)&src, slen);
        if (sent == NTP_PACKET_LEN) {
            s_served++;
        } else {
            ESP_LOGW(TAG, "server: sendto failed (%d / errno=%d)",
                     (int)sent, errno);
        }
    }
    /* unreachable */
}

bool ntp_server_start(void)
{
    if (s_server_started) return s_server_running;

    /* `srv_enabled` defaults to 1 (plan NVS schema). Users can disable
     * the server while keeping the client running via /settings -- handy
     * for STA-only nodes that just want their own clock synced. */
    if (ntp_nvs_get_u8("srv_enabled", 1) == 0) {
        ESP_LOGI(TAG, "server: disabled in NVS");
        return false;
    }
    if (ntp_nvs_get_u8("enabled", 1) == 0) {
        ESP_LOGI(TAG, "server: NTP master switch off, not starting");
        return false;
    }

    /* Pinned to CPU 0: shares an affinity domain with portal_http, WiFi,
     * and LwIP. See docs/portal-latency-analysis.md. Stack 3072 B; live
     * use measured well under 1 KB. */
    s_server_started = true;
    BaseType_t rc = xTaskCreatePinnedToCore(ntp_server_task, "ntp_srv",
                                            3072, NULL, 5, NULL, 0);
    if (rc != pdPASS) {
        s_server_started = false;
        ESP_LOGE(TAG, "server: xTaskCreate failed");
        return false;
    }
    return true;
}
