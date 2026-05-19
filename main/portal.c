/*
 * Captive Portal — HTTP server + DNS hijack for WiFi configuration
 *
 * Serves a web UI on port 80 for:
 *   - WiFi configuration (STA credentials, AP mode)
 *   - MQTT broker settings (port, auth)
 *   - AP settings (SSID, password)
 *   - Broker stats (clients, subs, retained, uptime, heap)
 *   - Device info (MAC, chip, PSRAM, flash)
 *   - Firmware version display (Tasmota-style)
 *   - OTA firmware upload (file upload and URL fetch)
 *   - Reboot
 *
 * DNS server hijacks all queries to the AP IP (configurable, default 192.168.25.1).
 */

#include "portal.h"
#include "mqtt_broker.h"
#include "wifi_connect.h"
#include "ntp.h"
#include "version.h"
#include "csrf.h"
#include "timers.h"
#include "tz_presets.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>   /* gmtime_r/strftime for the /time page (Phase 3 NTP) */
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"  /* esp_timer_get_time() for the per-request access log */
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "esp_flash.h"
#include "esp_wifi.h"
#include "nvs.h"

#ifdef CONFIG_MQTT_BROKER_ETHERNET
#include "eth_connect.h"
#endif

#include "portal_ws.h"
#include "portal_berry.h"
#include "berry_runtime.h"   /* BERRY_SLOT_COUNT for /berry/slot/N dispatch */

static const char *TAG = "portal";

#define PORTAL_HTTP_PORT    80
#define PORTAL_DNS_PORT     53
#define PORTAL_TASK_STACK   12288
#define NVS_SETTINGS_NS     "mqtt_cfg"

/* Portal state — shared with wifi_connect.c */
char s_portal_ssid[33] = "";
char s_portal_password[65] = "";
int  s_portal_ap_mode = 0;

static int s_http_fd = -1;
static int s_dns_fd = -1;

void portal_set_portal_ssid(const char *ssid)
{
    if (ssid) {
        strncpy(s_portal_ssid, ssid, sizeof(s_portal_ssid) - 1);
        s_portal_ssid[sizeof(s_portal_ssid) - 1] = '\0';
    }
}

/* ---- Sort comparators ---- */

/* qsort comparator for broker_client_info_t: ascending by last IP octet.
 * Unparseable IPs sort to 0. */
static int cmp_client_by_last_octet(const void *pa, const void *pb)
{
    const broker_client_info_t *a = (const broker_client_info_t *)pa;
    const broker_client_info_t *b = (const broker_client_info_t *)pb;
    const char *da = strrchr(a->ip, '.');
    const char *db = strrchr(b->ip, '.');
    int oa = da ? atoi(da + 1) : 0;
    int ob = db ? atoi(db + 1) : 0;
    return oa - ob;
}

/* ---- NVS settings helpers ---- */

static void nvs_settings_get_str(const char *key, char *out, size_t out_size, const char *def)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
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

static uint16_t nvs_settings_get_u16(const char *key, uint16_t def)
{
    nvs_handle_t h;
    uint16_t val = def;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, key, &val);
        nvs_close(h);
    }
    return val;
}

static uint8_t nvs_settings_get_u8(const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t val = def;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    return val;
}

static int32_t nvs_settings_get_i32(const char *key, int32_t def)
{
    nvs_handle_t h;
    int32_t val = def;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &val);
        nvs_close(h);
    }
    return val;
}

static void nvs_settings_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_settings_set_u16(const char *key, uint16_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_settings_set_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_settings_set_i32(const char *key, int32_t val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- Hostname helpers ---- */

#ifndef CONFIG_MQTT_BROKER_HOSTNAME
#define CONFIG_MQTT_BROKER_HOSTNAME "mqtt_broker"
#endif

void portal_get_hostname(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    /* Default from Kconfig */
    strncpy(out, CONFIG_MQTT_BROKER_HOSTNAME, out_size - 1);
    out[out_size - 1] = '\0';
    /* NVS override ("mqtt_cfg" / "hostname") if non-empty */
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) == ESP_OK) {
        char buf[33] = "";
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "hostname", buf, &len) == ESP_OK && buf[0] != '\0') {
            strncpy(out, buf, out_size - 1);
            out[out_size - 1] = '\0';
        }
        nvs_close(h);
    }
}

/* Validate hostname: 1-32 chars, [A-Za-z0-9_-], no leading/trailing '-'. */
static int hostname_is_valid(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    if (n == 0 || n > 32) return 0;
    if (s[0] == '-' || s[n - 1] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return 0;
    }
    return 1;
}

/* ---- /timers helpers (P0/P1 0.8.1 UX fixes) ----
 *
 * Format "YYYY-MM-DD HH:MM:SS (UTC±HH:MM)" using the device's POSIX TZ.
 * Uses strftime's %z (-0700) and rewrites it as (UTC-07:00). Numeric
 * offset is unambiguous regardless of the TZ name (`UTC7`, `PST8PDT`,
 * `<+0530>-5:30`, ...). Writes "clock not synced" when SNTP is dark.
 */
static void portal_format_local_time(time_t now_utc, char *out, size_t out_size)
{
    if (now_utc < 1700000000) {
        snprintf(out, out_size, "clock not synced");
        return;
    }
    struct tm lt;
    localtime_r(&now_utc, &lt);
    char ts[32], offz[8];
    strftime(ts,   sizeof(ts),   "%Y-%m-%d %H:%M:%S", &lt);
    strftime(offz, sizeof(offz), "%z", &lt);
    /* offz is "+HHMM" or "-HHMM". Convert to "+HH:MM" or fall back to
     * a plain numeric offset if strftime didn't yield 5 chars. */
    if (strlen(offz) == 5) {
        snprintf(out, out_size, "%s (UTC%c%c%c:%c%c)",
                 ts, offz[0], offz[1], offz[2], offz[3], offz[4]);
    } else {
        snprintf(out, out_size, "%s (UTC)", ts);
    }
}

/* Render "next_fire_unix" as a human relative + absolute local line for
 * the edit page. `slot_1based` is the slot we're editing. Writes "Not
 * scheduled" when next_fire_unix == 0 (disarmed / no days / clock dark).
 */
static void portal_format_next_fire(int slot_1based, char *out, size_t out_size)
{
    int64_t nx = timers_next_fire_unix(slot_1based);
    if (nx <= 0) {
        snprintf(out, out_size, "Not scheduled");
        return;
    }
    time_t now = time(NULL);
    int64_t delta = nx - (int64_t)now;
    if (delta < 0) delta = 0;
    int days = (int)(delta / 86400);
    int hours = (int)((delta % 86400) / 3600);
    int mins  = (int)((delta % 3600) / 60);

    struct tm lt;
    time_t nxt = (time_t)nx;
    localtime_r(&nxt, &lt);
    char hhmm[8], offz[8];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &lt);
    strftime(offz, sizeof(offz), "%z",    &lt);

    /* "today", "tomorrow", else day-of-week + date. */
    struct tm now_lt;
    localtime_r(&now, &now_lt);
    int dyd = lt.tm_yday - now_lt.tm_yday;
    /* Year wrap when computing on Dec 31 → Jan 1 etc. */
    if (lt.tm_year != now_lt.tm_year) dyd = 2;
    const char *day_label;
    char buf[24];
    if (dyd == 0) day_label = "today";
    else if (dyd == 1) day_label = "tomorrow";
    else { strftime(buf, sizeof(buf), "%a %b %-d", &lt); day_label = buf; }

    /* Relative "in 5h 12m" / "in 3d 2h". */
    char rel[24];
    if (days >= 1) snprintf(rel, sizeof(rel), "in %dd %dh", days, hours);
    else if (hours >= 1) snprintf(rel, sizeof(rel), "in %dh %dm", hours, mins);
    else snprintf(rel, sizeof(rel), "in %dm", mins);

    snprintf(out, out_size, "%s at %s (%s)", day_label, hhmm, rel);
}

/* ---- HTTP Request Parser ---- */

typedef enum {
    REQ_GET,
    REQ_POST,
    REQ_PUT,
    REQ_DELETE
} http_method_t;

typedef struct {
    http_method_t method;
    char path[128];
    char query[128];      /* URL query string after '?' (empty if none) */
    char body[512];
    char auth_user[65];   /* decoded from Authorization: Basic header */
    char auth_pass[65];
    /* CSRF token value from the X-CSRF-Token request header. Empty
     * string when absent. Form-submitted tokens (hidden input fields)
     * are NOT copied here -- those are extracted lazily from `body`
     * at validation time via csrf_verify(). */
    char csrf_header[CSRF_TOKEN_BUF_SIZE];
} http_request_t;

/* Minimal base64 decode for Basic auth */
static int base64_decode(const char *in, size_t in_len, char *out, size_t out_size)
{
    static const int8_t t[256] = {
        [0 ... 255] = -1,
        ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,
        ['G'] = 6,  ['H'] = 7,  ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11,
        ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17,
        ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
        ['Y'] = 24, ['Z'] = 25,
        ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
        ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37,
        ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43,
        ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
        ['y'] = 50, ['z'] = 51,
        ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55, ['4'] = 56, ['5'] = 57,
        ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63,
    };
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        uint32_t n = 0;
        int pad = 0;
        for (int j = 0; j < 4 && (i + j) < in_len; j++) {
            char c = in[i + j];
            if (c == '=') { pad++; n <<= 6; }
            else if (t[(uint8_t)c] >= 0) { n = (n << 6) | t[(uint8_t)c]; }
            else { n <<= 6; }
        }
        if (o < out_size) out[o++] = (n >> 16) & 0xFF;
        if (o < out_size && pad < 2) out[o++] = (n >> 8) & 0xFF;
        if (o < out_size && pad < 1) out[o++] = n & 0xFF;
    }
    if (o < out_size) out[o] = '\0';
    return (int)o;
}

static void http_parse(const uint8_t *data, size_t len, http_request_t *req)
{
    memset(req, 0, sizeof(*req));

    char *line = (char *)data;
    char *space = strchr(line, ' ');
    if (!space) return;
    *space = '\0';

    if (strncmp(line, "GET", 3) == 0) {
        req->method = REQ_GET;
    } else if (strncmp(line, "DELETE", 6) == 0) {
        req->method = REQ_DELETE;
    } else if (strncmp(line, "PUT", 3) == 0) {
        req->method = REQ_PUT;
    } else if (strncmp(line, "POST", 4) == 0) {
        req->method = REQ_POST;
    } else {
        return;
    }

    char *path = space + 1;
    space = strchr(path, ' ');
    if (!space) return;
    *space = '\0';

    char *qmark = strchr(path, '?');
    if (qmark) {
        *qmark = '\0';
        strncpy(req->query, qmark + 1, sizeof(req->query) - 1);
        req->query[sizeof(req->query) - 1] = '\0';
    } else {
        req->query[0] = '\0';
    }

    strncpy(req->path, path, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';

    char *body_start = strstr(space + 1, "\r\n\r\n");
    if (body_start && (req->method == REQ_POST || req->method == REQ_PUT)) {
        body_start += 4;
        size_t body_len = strlen(body_start);
        if (body_len > sizeof(req->body) - 1) {
            body_len = sizeof(req->body) - 1;
        }
        strncpy(req->body, body_start, body_len);
        req->body[body_len] = '\0';
    }

    /* Extract X-CSRF-Token header (case-insensitive). Tolerates either
     * canonical capitalization or the lowercase form some HTTP libs
     * emit. Value capped at CSRF_TOKEN_HEX_LEN; anything longer is
     * truncated and will fail constant-time compare downstream. */
    {
        char *csrf = strstr(space + 1, "X-CSRF-Token: ");
        if (!csrf) csrf = strstr(space + 1, "x-csrf-token: ");
        if (csrf) {
            csrf += 14;  /* skip "X-CSRF-Token: " */
            char *end = strstr(csrf, "\r\n");
            if (end) {
                size_t n = (size_t)(end - csrf);
                if (n >= sizeof(req->csrf_header)) {
                    n = sizeof(req->csrf_header) - 1;
                }
                memcpy(req->csrf_header, csrf, n);
                req->csrf_header[n] = '\0';
            }
        }
    }

    /* Extract Authorization: Basic header */
    char *auth = strstr(space + 1, "Authorization: Basic ");
    if (!auth) auth = strstr(space + 1, "authorization: Basic ");
    if (auth) {
        auth += 21;  /* skip "Authorization: Basic " */
        char *end = strstr(auth, "\r\n");
        if (end) {
            size_t b64_len = (size_t)(end - auth);
            char decoded[132];
            base64_decode(auth, b64_len, decoded, sizeof(decoded));
            /* Split user:pass */
            char *colon = strchr(decoded, ':');
            if (colon) {
                *colon = '\0';
                strncpy(req->auth_user, decoded, sizeof(req->auth_user) - 1);
                strncpy(req->auth_pass, colon + 1, sizeof(req->auth_pass) - 1);
            }
        }
    }
}

static const char *urldecode_param(const char *body, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) return NULL;
    out[0] = '\0';

    char buf[512];
    strncpy(buf, body, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, "&");
    while (token) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            if (strcmp(token, key) == 0) {
                char *val = eq + 1;
                size_t vi = 0, vo = 0;
                while (val[vi] && vo < out_size - 1) {
                    if (val[vi] == '%' && vi + 2 < strlen(val)) {
                        char hex[3] = { val[vi + 1], val[vi + 2], '\0' };
                        out[vo++] = (char)strtol(hex, NULL, 16);
                        vi += 3;
                    } else if (val[vi] == '+') {
                        out[vo++] = ' ';
                        vi++;
                    } else {
                        out[vo++] = val[vi++];
                    }
                }
                out[vo] = '\0';
                return out;
            }
        }
        token = strtok(NULL, "&");
    }
    return NULL;
}

/* ---- HTTP Response Builder ---- */

static int http_response_start(int fd, const char *status, const char *content_type, size_t body_len)
{
    /* Always re-emit the CSRF cookie on every response. The token is
     * constant per boot, so this is idempotent for the browser. ~70 B
     * overhead per response, dwarfed by the response body itself.
     *
     * Cookie attributes:
     *   Path=/             -- scope to the whole site (every endpoint)
     *   SameSite=Strict    -- block cross-site cookie sends; primary
     *                         CSRF defense even before the token check
     *   (NOT HttpOnly)     -- client JS must read it for the
     *                         X-CSRF-Token header in fetch() calls.
     *                         Acceptable here: every dynamic value
     *                         injected into HTML goes through textContent
     *                         on the client and through HTML-encoding
     *                         server-side, so XSS surface is effectively
     *                         zero. Token disclosure to a same-origin
     *                         attacker is moot -- they'd already be in.
     *   (NOT Secure)       -- portal is HTTP-only; setting Secure would
     *                         silently drop the cookie. */
    char header[384];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "Set-Cookie: csrf=%s; Path=/; SameSite=Strict\r\n"
        "\r\n",
        status, content_type, (unsigned long)body_len,
        csrf_token_hex());

    ssize_t sent = 0;
    while ((size_t)sent < (size_t)len) {
        ssize_t n = send(fd, header + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int http_send_body(int fd, const char *body, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, body + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int http_send_redirect(int fd, const char *url)
{
    char body[256];
    int len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Redirect</title></head>"
        "<body style='margin:20px;font-family:sans-serif'>"
        "<script>window.location.href='%s';</script>"
        "<p>Redirecting... <a href='%s'>Click here</a></p>"
        "</body></html>", url, url);

    if (http_response_start(fd, "302 Found", "text/html; charset=utf-8", len) < 0) return -1;
    return http_send_body(fd, body, len);
}

/*
 * Reboot-countdown page.
 *
 * Sent right before the device intentionally goes away (manual reboot, OTA
 * upload success, OTA URL success, OTA rollback). Replaces the old
 * "Rebooting..." plain page that dumped the user on a broken socket and
 * forced them to manually re-navigate.
 *
 * The page polls /api/status every 1s with cache:'no-store' and tracks
 * three signals:
 *   1. The first request that fails or times out  -> the device went down.
 *      We only believe "back online" responses that come AFTER this edge,
 *      so a response that arrives before the reboot starts is ignored.
 *   2. uptime_s coming back smaller than the baseline reading -> definite
 *      fresh boot, even if (1) was missed (e.g. very fast reboot).
 *   3. A successful response after (1) or (2) -> safe to show the green
 *      "Back online" pill and link the user to `return_path`.
 *
 * Falls back to `<noscript><meta http-equiv='refresh' content='15;url=...'>`
 * for JS-disabled clients.
 *
 * Total page weight: ~2.2 KB (inline CSS + JS). Stored on the heap-allocated
 * page buffer the caller already owns; this helper never allocates itself.
 */
static int http_send_reboot_countdown(int fd,
                                      const char *title,
                                      const char *subtitle,
                                      const char *return_path)
{
    /* 4 KB is plenty for the page; we don't want to grow stack here. */
    char *body = (char *)malloc(4096);
    if (!body) return -1;

    int len = snprintf(body, 4096,
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title>"
        "<noscript><meta http-equiv='refresh' content='15;url=%s'></noscript>"
        "<style>"
        "body{margin:0;padding:40px 20px;font-family:verdana,sans-serif;"
        "background:#252525;color:#eaeaea;text-align:center}"
        ".card{display:inline-block;min-width:300px;max-width:480px;"
        "background:#1f1f1f;border:1px solid #555;border-radius:6px;padding:24px}"
        "h2{margin:0 0 4px;color:#1fa3ec;font-size:1.3em}"
        "p{margin:0.4em 0;color:#ccc}"
        ".sub{font-size:0.9em;color:#aaa}"
        ".elapsed{font-family:monospace;font-size:0.85em;color:#888;margin-top:14px}"
        ".dot{display:inline-block;width:10px;height:10px;border-radius:50%%;"
        "background:#e65100;margin-right:8px;vertical-align:-1px;"
        "animation:pulse 1s infinite}"
        ".dot.ok{background:#47c266;animation:none}"
        "@keyframes pulse{0%%,100%%{opacity:1}50%%{opacity:.35}}"
        ".btn{display:inline-block;margin-top:16px;padding:10px 18px;"
        "background:#47c266;color:#fff;border-radius:0.3rem;text-decoration:none;"
        "font-size:1.05em}"
        ".btn:hover{background:#2d9e46;color:#fff}"
        ".btn.hidden{display:none}"
        ".bar{background:#444;height:4px;border-radius:2px;margin-top:14px;overflow:hidden}"
        ".bar>div{height:100%%;width:0;background:#1fa3ec;transition:width .3s}"
        "</style></head><body>"
        "<div class='card'>"
        "<h2>%s</h2>"
        "<p class='sub'>%s</p>"
        "<p id='state'><span class='dot' id='dot'></span><span id='msg'>Waiting for device...</span></p>"
        "<div class='bar'><div id='bar'></div></div>"
        "<p class='elapsed' id='elapsed'>0s elapsed</p>"
        "<a id='go' class='btn hidden' href='%s'>Continue \xe2\x86\x92</a>"
        "</div>"
        "<script>(function(){"
        /* Track the offline edge and uptime baseline so we don't falsely
         * claim "back online" from the pre-reboot in-flight response. */
        "var seenOffline=false,baselineUptime=null,start=Date.now();"
        "var msg=document.getElementById('msg'),dot=document.getElementById('dot');"
        "var go=document.getElementById('go'),bar=document.getElementById('bar');"
        "var elapsed=document.getElementById('elapsed');"
        "var done=false;"
        "function setProgress(){"
        "var s=Math.floor((Date.now()-start)/1000);"
        "elapsed.textContent=s+'s elapsed';"
        /* Expect ~8-15s typical reboot. Cap visual fill at 90 percent until
         * we've actually seen it come back. */
        "if(!done){bar.style.width=Math.min(90,s*7)+'%%';}"
        "}"
        "function backOnline(){if(done)return;done=true;"
        "bar.style.width='100%%';dot.className='dot ok';"
        "msg.textContent='Back online \xe2\x80\x94 redirecting...';go.classList.remove('hidden');"
        /* Redirect home as soon as the device responds again. 400ms is
         * enough for the green pill to register visually but short enough
         * that users don't perceive a lag. */
        "setTimeout(function(){window.location.href=go.href;},400);}"
        "function tick(){setProgress();"
        "var ctrl=('AbortController' in window)?new AbortController():null;"
        "var to=setTimeout(function(){if(ctrl)ctrl.abort();},1500);"
        /* Polls /api/ping (unauthenticated, returns just uptime). Treats
         * any HTTP response as 'device is alive' -- so even if auth comes
         * back on a non-exempt endpoint we'd still flip green correctly.
         * credentials:'omit' is belt-and-braces so no browser-level auth
         * prompt can fire from this fetch even if the URL were ever moved. */
        "fetch('/api/ping',{cache:'no-store',credentials:'omit',signal:ctrl?ctrl.signal:undefined})"
        ".then(function(r){clearTimeout(to);"
        "return r.json().then(function(d){return{ok:true,d:d};},function(){return{ok:true,d:null};});})"
        ".then(function(res){"
        "var up=res.d&&typeof res.d.uptime_s==='number'?res.d.uptime_s:null;"
        "if(baselineUptime===null&&up!==null){baselineUptime=up;}"
        "if(seenOffline||(up!==null&&baselineUptime!==null&&up<baselineUptime)){"
        "backOnline();return;}"
        "msg.textContent='Device still up, waiting for reboot...';"
        "})"
        ".catch(function(){clearTimeout(to);seenOffline=true;"
        "msg.textContent='Device offline, will resume when it returns...';});"
        "if(!done)setTimeout(tick,1000);}"
        /* Tiny initial delay so the very first poll doesn't race the
         * 800ms vTaskDelay before esp_restart(). */
        "setTimeout(tick,400);"
        "})();</script>"
        "</body></html>",
        title, return_path,
        title, subtitle, return_path);

    if (len < 0 || len >= 4096) { free(body); return -1; }

    int rc = http_response_start(fd, "200 OK", "text/html; charset=utf-8", (size_t)len);
    if (rc == 0) rc = http_send_body(fd, body, (size_t)len);
    free(body);
    return rc;
}

static void http_send_page(int fd, const char *body_content, size_t body_len)
{
    static const char *hdr =
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>MQTT Broker</title><style>"
        "div,fieldset,input,select{padding:5px;font-size:1em}"
        "fieldset{background:#1f1f1f;margin:8px 0;border:1px solid #555;border-radius:4px}"
        "legend{font-weight:bold;padding:0 6px}"
        "form{display:block;margin:4px 0}"
        "p{margin:0.5em 0}"
        "input{width:100%;box-sizing:border-box;background:#1f1f1f;color:#eaeaea;"
        "border:1px solid #555;border-radius:0.3rem;padding:8px}"
        "input[type=checkbox],input[type=radio]{width:1em;margin-right:6px;vertical-align:-1px}"
        "body{text-align:center;font-family:verdana,sans-serif;background:#252525;color:#eaeaea}"
        "td{padding:0px}"
        "button,.btn{border:0;border-radius:0.3rem;background:#1fa3ec;color:#faffff;"
        "line-height:2.4rem;font-size:1.2rem;width:100%;"
        "transition-duration:0.4s;cursor:pointer;box-sizing:border-box}"
        /* Anchor-as-button: used for plain GET navigation so the URL
         * doesn't pick up a trailing '?' the way an empty <form> would. */
        ".btn{display:block;margin:4px 0;text-align:center;text-decoration:none;"
        "padding:0;font-family:inherit}"
        "button:hover,.btn:hover{background:#0e70a4;color:#faffff}"
        ".btn:visited{color:#faffff}"
        ".bred{background:#d43535}.bred:hover{background:#931f1f}"
        ".bgrn{background:#47c266}.bgrn:hover{background:#2d9e46}"
        ".bgry{background:#888}.bgry:hover{background:#666}"
        "a{color:#1fa3ec;text-decoration:none}"
        "table{width:100%;border-collapse:collapse}"
        "th,td{padding:4px 6px;text-align:left}"
        "th{color:#999;font-weight:normal;width:45%}"
        "td{color:#eaeaea}"
        "h2{font-size:1.1em;color:#1fa3ec;margin:0}"
        "h3{font-size:0.85em;color:#aaa;margin:0}"
        "label{display:block;font-size:0.9em;margin:6px 0 2px;color:#ccc}"
        ".st{padding:6px 10px;border-radius:4px;margin:6px 0;font-size:0.9em;text-align:center}"
        ".ok{background:#1b5e20;color:#a5d6a7}"
        ".warn{background:#e65100;color:#ffcc80}"
        "</style></head><body>"
        "<div style='text-align:left;display:inline-block;color:#eaeaea;"
        "min-width:340px;position:relative;'>"
        "<div style='text-align:center;color:#1fa3ec;'>"
        "<h2>MQTT Broker</h2><h3>ESP32-S3</h3></div>";
    static const char *ftr =
        "<div style='text-align:right;font-size:11px;color:#aaa;'><hr style='border:0;border-top:1px solid #555'>"
        FW_FOOTER "</div>"
        "</div></body></html>";

    size_t hdr_len = strlen(hdr);
    size_t ftr_len = strlen(ftr);
    size_t total = hdr_len + body_len + ftr_len;

    char *html = (char *)malloc(total + 1);
    if (!html) return;
    memcpy(html, hdr, hdr_len);
    memcpy(html + hdr_len, body_content, body_len);
    memcpy(html + hdr_len + body_len, ftr, ftr_len);
    html[total] = '\0';

    http_response_start(fd, "200 OK", "text/html; charset=utf-8", total);
    http_send_body(fd, html, total);
    free(html);
}

/* ---- OTA streaming upload handler ---- */

static void http_send_plain(int fd, const char *status, const char *msg)
{
    size_t len = strlen(msg);
    http_response_start(fd, status, "text/plain", len);
    http_send_body(fd, msg, len);
}

/* Sends a 403 with a short HTML body explaining the CSRF rejection.
 * Used as the single failure response for every state-changing
 * endpoint. The body is intentionally short and stable so test
 * harnesses can assert on it.
 *
 * 403 (not 401) is correct here: 401 means "you need credentials"
 * which would re-trigger the browser's basic-auth dialog. The user has
 * already authenticated; what's missing is the CSRF token. 403
 * 'Forbidden' is the right semantic and avoids the auth-dialog loop. */
static void http_send_csrf_403(int fd)
{
    const char *msg =
        "<html><body><h1>403 Forbidden</h1>"
        "<p>Missing or invalid CSRF token.</p>"
        "<p>If you reached this page from a bookmark or a form"
        " submitted before the device rebooted, reload the page"
        " once to refresh your CSRF token, then retry.</p>"
        "<p><a href='/'>Back to dashboard</a></p>"
        "</body></html>";
    size_t len = strlen(msg);
    http_response_start(fd, "403 Forbidden", "text/html; charset=utf-8", len);
    http_send_body(fd, msg, len);
}

/*
 * Handle multipart/form-data OTA upload.
 * Called when we detect POST /ota-upload in the initial header recv.
 * `header_buf` contains the first chunk already read (including the HTTP headers
 * and possibly the start of the body).  `header_len` is how many bytes are in it.
 */
static void handle_ota_upload(int client_fd, char *header_buf, int header_len)
{
    ESP_LOGI(TAG, "OTA upload starting");

    /* CSRF check up front, before we commit to reading megabytes of
     * body bytes. Multipart uploads carry the token via either:
     *   - URL query string  /ota-upload?csrf=<hex>      (browser <form>)
     *   - X-CSRF-Token: <hex> request header           (XHR + curl)
     * Hidden-input <form> fields land late in the multipart stream
     * (browser serializes inputs in DOM order, with the file last),
     * so we'd have to buffer the entire upload before deciding to
     * accept it -- avoidable by carrying the token in the URL. */
    {
        char csrf_val[CSRF_TOKEN_BUF_SIZE] = "";

        /* Extract X-CSRF-Token header. */
        char *h = strstr(header_buf, "X-CSRF-Token: ");
        if (!h) h = strstr(header_buf, "x-csrf-token: ");
        if (h) {
            h += 14;  /* skip "X-CSRF-Token: " */
            char *end = strstr(h, "\r\n");
            if (end) {
                size_t n = (size_t)(end - h);
                if (n >= sizeof(csrf_val)) n = sizeof(csrf_val) - 1;
                memcpy(csrf_val, h, n);
                csrf_val[n] = '\0';
            }
        }

        /* Fall back to the URL query string `?csrf=<hex>` or
         * `&csrf=<hex>`. The first line of header_buf is the request
         * line ("POST /ota-upload?csrf=ABC... HTTP/1.1\r\n"); we scan
         * up to the first \r\n only to avoid matching headers below. */
        if (csrf_val[0] == '\0') {
            char *req_line_end = strstr(header_buf, "\r\n");
            if (req_line_end) {
                *req_line_end = '\0';  /* temporary terminator */
                char *q = strstr(header_buf, "csrf=");
                if (q && (q == header_buf ||
                          *(q - 1) == '?' || *(q - 1) == '&')) {
                    q += 5;
                    /* stop at '&', ' ' (before HTTP/1.1), or end */
                    size_t n = 0;
                    while (q[n] && q[n] != '&' && q[n] != ' ' &&
                           n < sizeof(csrf_val) - 1) {
                        csrf_val[n] = q[n];
                        n++;
                    }
                    csrf_val[n] = '\0';
                }
                *req_line_end = '\r';  /* restore so downstream parsers see it */
            }
        }

        if (!csrf_verify(csrf_val, NULL)) {
            ESP_LOGW(TAG, "OTA upload rejected: missing or invalid CSRF token");
            http_send_csrf_403(client_fd);
            return;
        }
    }

    /* Find Content-Length */
    char *cl_hdr = strstr(header_buf, "Content-Length: ");
    if (!cl_hdr) cl_hdr = strstr(header_buf, "content-length: ");
    if (!cl_hdr) {
        http_send_plain(client_fd, "400 Bad Request", "Missing Content-Length");
        return;
    }
    int content_length = atoi(cl_hdr + 16);
    if (content_length <= 0 || content_length > 4 * 1024 * 1024) {
        http_send_plain(client_fd, "400 Bad Request", "Invalid size (max 4MB)");
        return;
    }

    /* Find the boundary from Content-Type header */
    char boundary[128] = "";
    char *ct = strstr(header_buf, "Content-Type: multipart/form-data");
    if (!ct) ct = strstr(header_buf, "content-type: multipart/form-data");
    if (ct) {
        char *bnd = strstr(ct, "boundary=");
        if (bnd) {
            bnd += 9;
            char *end = strstr(bnd, "\r\n");
            if (end) {
                size_t blen = (size_t)(end - bnd);
                if (blen > sizeof(boundary) - 3) blen = sizeof(boundary) - 3;
                boundary[0] = '-'; boundary[1] = '-';  /* multipart boundaries are prefixed with -- */
                memcpy(boundary + 2, bnd, blen);
                boundary[2 + blen] = '\0';
            }
        }
    }
    if (boundary[0] == '\0') {
        http_send_plain(client_fd, "400 Bad Request", "Missing multipart boundary");
        return;
    }

    /* Find start of body (after \r\n\r\n) */
    char *body_start = strstr(header_buf, "\r\n\r\n");
    if (!body_start) {
        http_send_plain(client_fd, "400 Bad Request", "Malformed request");
        return;
    }
    body_start += 4;
    int body_in_header = header_len - (int)(body_start - header_buf);

    /* We need to skip the multipart headers (boundary + Content-Disposition + empty line)
     * to get to the actual binary data. Scan what we have. */
    char *bin_start = NULL;

    /* Look for the double \r\n\r\n after the multipart part headers */
    char *part_hdr_end = NULL;
    if (body_in_header > 0) {
        part_hdr_end = strstr(body_start, "\r\n\r\n");
    }

    int body_received = body_in_header;  /* total body bytes received so far */

    /* Begin OTA */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        http_send_plain(client_fd, "500 Internal Server Error", "No OTA partition available");
        return;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        http_send_plain(client_fd, "500 Internal Server Error", "OTA begin failed");
        return;
    }

    ESP_LOGI(TAG, "OTA partition: %s, content_length=%d", update_part->label, content_length);

    /* Allocate a recv buffer for streaming */
    #define OTA_BUF_SIZE 4096
    char *recv_buf = (char *)malloc(OTA_BUF_SIZE);
    if (!recv_buf) {
        esp_ota_abort(ota_handle);
        http_send_plain(client_fd, "500 Internal Server Error", "Out of memory");
        return;
    }

    bool header_skipped = (part_hdr_end != NULL);
    size_t total_written = 0;

    /* If we already found the binary start in the header buffer, write that part */
    if (header_skipped) {
        bin_start = part_hdr_end + 4;
        int initial_bin = body_in_header - (int)(bin_start - body_start);
        if (initial_bin > 0) {
            err = esp_ota_write(ota_handle, bin_start, initial_bin);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
                esp_ota_abort(ota_handle);
                free(recv_buf);
                http_send_plain(client_fd, "500 Internal Server Error", "OTA write failed");
                return;
            }
            total_written += initial_bin;
        }
    } else if (body_in_header > 0) {
        /* We have some body data but haven't found the part header end yet.
         * Buffer it and keep scanning. */
        /* For simplicity, copy remaining body data to recv_buf */
        int to_copy = body_in_header;
        if (to_copy > OTA_BUF_SIZE) to_copy = OTA_BUF_SIZE;
        memcpy(recv_buf, body_start, to_copy);
    }

    /* Stream remaining body data from socket */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (body_received < content_length) {
        int to_read = content_length - body_received;
        if (to_read > OTA_BUF_SIZE) to_read = OTA_BUF_SIZE;

        ssize_t n = recv(client_fd, recv_buf, to_read, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "OTA recv error: %d (errno=%d)", (int)n, errno);
            break;
        }
        body_received += (int)n;

        if (!header_skipped) {
            /* Still looking for the end of multipart part headers */
            /* Append and search — simplified: look for \r\n\r\n in current chunk */
            char *end = memmem(recv_buf, n, "\r\n\r\n", 4);
            if (end) {
                header_skipped = true;
                char *data = end + 4;
                int data_len = (int)n - (int)(data - recv_buf);
                if (data_len > 0) {
                    err = esp_ota_write(ota_handle, data, data_len);
                    if (err != ESP_OK) break;
                    total_written += data_len;
                }
                continue;
            }
            /* Haven't found header end yet, skip this chunk */
            continue;
        }

        /* Write binary data, but watch for trailing boundary */
        err = esp_ota_write(ota_handle, recv_buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            break;
        }
        total_written += n;
    }

    free(recv_buf);

    if (err != ESP_OK || body_received < content_length) {
        esp_ota_abort(ota_handle);
        http_send_plain(client_fd, "500 Internal Server Error", "OTA write/recv failed");
        return;
    }

    /* The last few bytes include the multipart closing boundary.
     * esp_ota_end will validate the image and reject garbage at the end
     * if it corrupts the image header. In practice, the trailing
     * boundary (~60 bytes) is harmless padding in the flash partition. */

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            http_send_plain(client_fd, "400 Bad Request",
                "Firmware validation failed — invalid image");
        } else {
            http_send_plain(client_fd, "500 Internal Server Error", "OTA finalize failed");
        }
        return;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        http_send_plain(client_fd, "500 Internal Server Error", "Failed to set boot partition");
        return;
    }

    ESP_LOGI(TAG, "OTA upload success: %zu bytes written to %s", total_written, update_part->label);
    http_send_plain(client_fd, "200 OK", "OTA success, rebooting...");
    close(client_fd);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/*
 * Handle OTA via URL — download firmware from an HTTP URL and flash it.
 */
static void handle_ota_url(int client_fd, const char *url)
{
    ESP_LOGI(TAG, "OTA URL: %s", url);

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        http_send_plain(client_fd, "500 Internal Server Error", "No OTA partition");
        return;
    }

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        http_send_plain(client_fd, "500 Internal Server Error", "HTTP client init failed");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        http_send_plain(client_fd, "502 Bad Gateway", "Cannot connect to URL");
        return;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP %d from firmware URL", status_code);
        esp_http_client_cleanup(client);
        char msg[64];
        snprintf(msg, sizeof(msg), "Server returned HTTP %d", status_code);
        http_send_plain(client_fd, "502 Bad Gateway", msg);
        return;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        http_send_plain(client_fd, "500 Internal Server Error", "OTA begin failed");
        return;
    }

    char *buf = (char *)malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_cleanup(client);
        http_send_plain(client_fd, "500 Internal Server Error", "Out of memory");
        return;
    }

    size_t total = 0;
    while (1) {
        int n = esp_http_client_read(client, buf, 4096);
        if (n < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            err = ESP_FAIL;
            break;
        }
        if (n == 0) break;  /* done */

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            break;
        }
        total += n;
    }

    free(buf);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        http_send_plain(client_fd, "500 Internal Server Error", "OTA download/write failed");
        return;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            http_send_plain(client_fd, "400 Bad Request", "Firmware validation failed");
        } else {
            http_send_plain(client_fd, "500 Internal Server Error", "OTA finalize failed");
        }
        return;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        http_send_plain(client_fd, "500 Internal Server Error", "Failed to set boot partition");
        return;
    }

    ESP_LOGI(TAG, "OTA URL success: %zu bytes from %s", total, url);

    /* Reboot-countdown page; subtitle includes byte count so users can
     * cross-check against the expected firmware size before the device
     * disappears. */
    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle),
             "Downloaded %zu bytes. Booting new firmware...", total);
    http_send_reboot_countdown(client_fd, "Firmware updated", subtitle, "/");
    close(client_fd);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ---- HTTP Handlers ---- */

static void handle_http_client(int client_fd)
{
    /* Per-request access log: capture start time so we can print method,
     * path, and elapsed-ms at every termination point. Helps diagnose the
     * 'sometimes slow' tail (see docs/portal-latency-analysis.md). The
     * method/path are filled in after http_parse() succeeds; until then
     * the log uses placeholders so even malformed/aborted requests get
     * accounted for. */
    int64_t req_start_us = esp_timer_get_time();
    const char *req_method = "?";
    const char *req_path = "?";

    char *buf = (char *)malloc(2048);
    if (!buf) {
        ESP_LOGW(TAG, "http  %s %s  ENOMEM  %lldms",
                 req_method, req_path,
                 (esp_timer_get_time() - req_start_us) / 1000);
        close(client_fd);
        return;
    }
    int total = 0;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read just the headers first (look for \r\n\r\n) */
    while (total < 2047) {
        ssize_t n = recv(client_fd, buf + total, 2047 - total, 0);
        if (n <= 0) break;
        total += (int)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (total <= 0) {
        ESP_LOGW(TAG, "http  %s %s  recv_fail  %lldms",
                 req_method, req_path,
                 (esp_timer_get_time() - req_start_us) / 1000);
        free(buf);
        close(client_fd);
        return;
    }

    /* Check for OTA upload early — before we consume the body.
     * For multipart POST to /ota-upload, hand off the socket to the
     * streaming OTA handler with the header buffer still intact. */
    if (strncmp(buf, "POST ", 5) == 0 && strstr(buf, "/ota-upload")) {
        /* Auth check — make a copy since http_parse modifies the buffer */
        char *buf_copy = (char *)malloc(total + 1);
        if (!buf_copy) { free(buf); close(client_fd); return; }
        memcpy(buf_copy, buf, total);
        buf_copy[total] = '\0';

        http_request_t auth_req;
        http_parse((const uint8_t *)buf_copy, (size_t)total, &auth_req);
        free(buf_copy);

        char cfg_user[65] = "";
        char cfg_pass[65] = "";
        nvs_settings_get_str("auth_user", cfg_user, sizeof(cfg_user), "");
        nvs_settings_get_str("auth_pass", cfg_pass, sizeof(cfg_pass), "");

        if (cfg_user[0] != '\0' && cfg_pass[0] != '\0') {
            bool auth_ok = (strcmp(auth_req.auth_user, cfg_user) == 0 &&
                            strcmp(auth_req.auth_pass, cfg_pass) == 0);
            if (!auth_ok) {
                const char *resp =
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"MQTT Broker\"\r\n"
                    "Content-Length: 12\r\n"
                    "Connection: close\r\n\r\nUnauthorized";
                send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
                free(buf);
                close(client_fd);
                return;
            }
        }

        handle_ota_upload(client_fd, buf, total);
        free(buf);
        close(client_fd);
        return;
    }

    /* ---- Berry script POST early-intercept ----
     * /berry/save, /berry/eval, and /berry/slot/N/save bodies may exceed
     * http_request_t.body's 512-byte cap (scripts are up to BERRY_SCRIPT_MAX).
     * Hand off to the streaming handler before http_parse truncates.
     * Auth is enforced here; portal_berry_handle_post_stream does CSRF. */
    if (strncmp(buf, "POST ", 5) == 0 &&
        (strstr(buf, " /berry/save ") || strstr(buf, " /berry/save?") ||
         strstr(buf, " /berry/eval ") || strstr(buf, " /berry/eval?") ||
         strstr(buf, " /berry/slot/"))) {

        portal_berry_post_kind_t kind = PORTAL_BERRY_POST_EVAL;
        int slot = 0;
        if (strstr(buf, " /berry/save")) {
            kind = PORTAL_BERRY_POST_SAVE;
        } else if (strstr(buf, " /berry/slot/")) {
            kind = PORTAL_BERRY_POST_SLOT_SAVE;
            /* Extract slot number: /berry/slot/N/save */
            const char *p = strstr(buf, "/berry/slot/");
            if (p) slot = atoi(p + 12);
            if (slot < 0 || slot >= BERRY_SLOT_COUNT) slot = 0;
        }

        char *buf_copy = (char *)malloc(total + 1);
        if (!buf_copy) { free(buf); close(client_fd); return; }
        memcpy(buf_copy, buf, total);
        buf_copy[total] = '\0';

        http_request_t auth_req;
        http_parse((const uint8_t *)buf_copy, (size_t)total, &auth_req);
        free(buf_copy);

        char cfg_user[65] = "";
        char cfg_pass[65] = "";
        nvs_settings_get_str("auth_user", cfg_user, sizeof(cfg_user), "");
        nvs_settings_get_str("auth_pass", cfg_pass, sizeof(cfg_pass), "");

        if (cfg_user[0] != '\0' && cfg_pass[0] != '\0') {
            bool auth_ok = (strcmp(auth_req.auth_user, cfg_user) == 0 &&
                            strcmp(auth_req.auth_pass, cfg_pass) == 0);
            if (!auth_ok) {
                const char *resp =
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"MQTT Broker\"\r\n"
                    "Content-Length: 12\r\n"
                    "Connection: close\r\n\r\nUnauthorized";
                send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
                free(buf);
                close(client_fd);
                return;
            }
        }

        portal_berry_handle_post_stream(client_fd, buf, total, kind, slot);
        free(buf);
        return;  /* socket closed by the streaming handler */
    }

    /* ---- WebSocket upgrade for /tester ----
     * Detected on the raw header buffer (like OTA) because the upgrade
     * needs the original Sec-WebSocket-Key header which http_parse drops.
     * Auth is enforced here using the same NVS credentials as the rest of
     * the portal. portal_ws_handle_upgrade() takes ownership of client_fd
     * on success. */
    if (strncmp(buf, "GET ", 4) == 0 && strstr(buf, " /ws ") &&
        (strstr(buf, "Upgrade: websocket") || strstr(buf, "Upgrade:websocket") ||
         strstr(buf, "upgrade: websocket"))) {

        char *buf_copy = (char *)malloc(total + 1);
        if (!buf_copy) { free(buf); close(client_fd); return; }
        memcpy(buf_copy, buf, total);
        buf_copy[total] = '\0';

        http_request_t auth_req;
        http_parse((const uint8_t *)buf_copy, (size_t)total, &auth_req);
        free(buf_copy);

        char cfg_user[65] = "";
        char cfg_pass[65] = "";
        nvs_settings_get_str("auth_user", cfg_user, sizeof(cfg_user), "");
        nvs_settings_get_str("auth_pass", cfg_pass, sizeof(cfg_pass), "");

        if (cfg_user[0] != '\0' && cfg_pass[0] != '\0') {
            bool auth_ok = (strcmp(auth_req.auth_user, cfg_user) == 0 &&
                            strcmp(auth_req.auth_pass, cfg_pass) == 0);
            if (!auth_ok) {
                const char *resp =
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"MQTT Broker\"\r\n"
                    "Content-Length: 12\r\n"
                    "Connection: close\r\n\r\nUnauthorized";
                send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
                free(buf);
                close(client_fd);
                return;
            }
        }

        bool took = portal_ws_handle_upgrade(client_fd, buf, (size_t)total);
        free(buf);
        if (!took) {
            /* portal_ws_handle_upgrade closes the socket on failure. */
        }
        return;  /* either the WS task owns the fd now, or it's already closed */
    }

    http_request_t req;
    http_parse((const uint8_t *)buf, (size_t)total, &req);
    free(buf);  /* done with recv buffer */

    /* Now we have a parsed request -- update access-log identifiers. */
    req_method = (req.method == REQ_GET)    ? "GET"
                : (req.method == REQ_POST)   ? "POST"
                : (req.method == REQ_PUT)    ? "PUT"
                : (req.method == REQ_DELETE) ? "DELETE" : "?";
    req_path = req.path[0] ? req.path : "/";

    /* Allocate a shared page-body buffer on the heap */
    #define PAGE_BUF_SIZE 16384
    char *body = (char *)malloc(PAGE_BUF_SIZE);
    if (!body) { close(client_fd); return; }

    /* ---- HTTP Basic Auth check ---- */
    /* Uses the same auth_user / auth_pass as MQTT broker auth.
     * If both are empty in NVS, auth is disabled (open portal).
     *
     * EXEMPTIONS: /api/ping is the liveness endpoint used by the reboot-
     * countdown page's polling loop. Forcing auth on it caused two real
     * problems for users running with Basic Auth enabled:
     *   1. The browser's native auth dialog reopened repeatedly during
     *      polling, because some browsers drop cached basic creds across
     *      the network-error -> 401 -> 401 cycle that occurs while a
     *      device is rebooting (each 401 with a WWW-Authenticate header
     *      re-triggers the prompt unless creds are already in cache).
     *   2. fetch() with default credentials cannot suppress that prompt.
     * /api/ping returns only the uptime (no secrets, no settings), so
     * making it open is the right trade-off. The dashboard, settings,
     * and the /api/status / /api/clients endpoints with sensitive info
     * remain auth-gated as before. */
    {
        char cfg_user[65] = "";
        char cfg_pass[65] = "";
        nvs_settings_get_str("auth_user", cfg_user, sizeof(cfg_user), "");
        nvs_settings_get_str("auth_pass", cfg_pass, sizeof(cfg_pass), "");

        /* /api/ping  -- liveness, used by countdown polling (see above).
         * /api/time  -- read-only NTP state, used by clients that just want
         *               wall-clock without going through the broker. Per
         *               plan-ntp-server.md the GET is documented as open.
         *               No settings or secrets in the response. */
        bool auth_exempt = (strcmp(req.path, "/api/ping") == 0) ||
                           (strcmp(req.path, "/api/time") == 0 && req.method == REQ_GET);

        if (!auth_exempt && cfg_user[0] != '\0' && cfg_pass[0] != '\0') {
            /* Auth is enabled -- check credentials */
            bool auth_ok = (strcmp(req.auth_user, cfg_user) == 0 &&
                            strcmp(req.auth_pass, cfg_pass) == 0);
            if (!auth_ok) {
                const char *resp =
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"MQTT Broker\"\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: 58\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "<html><body><h1>401 Unauthorized</h1></body></html>";
                send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
                /* 401 is uncommon enough to always log -- helps users
                 * debug auth issues without enabling debug verbosity. */
                ESP_LOGW(TAG, "http  %s %s  401  %lldms",
                         req_method, req_path,
                         (esp_timer_get_time() - req_start_us) / 1000);
                free(body);
                close(client_fd);
                return;
            }
        }
    }

    /* ============ MQTT TESTER PAGE ============
     * Vanilla HTML + JS, no framework. Opens a WS to /ws on this host.
     * All payload/topic text is rendered via textContent to avoid XSS.
     * $SYS topics are hidden by default (toggle in UI). */
    if (strcmp(req.path, "/tester") == 0) {
        int pos = snprintf(body, PAGE_BUF_SIZE,
"<style>"
".tester{font-family:sans-serif}"
".row{display:flex;gap:8px;align-items:center;margin:4px 0;flex-wrap:wrap}"
".row label{min-width:80px}"
".row input[type=text],.row textarea{flex:1 1 200px;min-width:120px;padding:4px}"
/* The .btn rule in the page-level stylesheet pins anchors/buttons to
 * width:100%, which collapsed sibling inputs in the tester's flex rows to
 * a ~5px sliver. Constrain tester buttons specifically so the filter input
 * keeps a sane width. */
".tester .row button.btn{width:auto;flex:0 0 auto;padding:0 14px;line-height:2rem;font-size:1rem}"
".tester .row .opt{flex:0 0 auto;min-width:0}"
".st-conn{display:inline-block;padding:2px 8px;border-radius:3px;font-size:.85em}"
".st-on{background:#2a7;color:#fff}.st-off{background:#a33;color:#fff}"
".bc{font-size:.75em;color:#888;margin-left:6px}"
"#log{font-family:monospace;font-size:.85em;background:#1a1a1a;color:#eaeaea;"
"padding:8px;height:380px;overflow:auto;border:1px solid #444}"
"#log .msg{border-bottom:1px solid #333;padding:2px 0}"
"#log .t{color:#6cf}#log .ts{color:#888}#log .r{color:#f90}"
"#log pre{margin:2px 0 0 12px;white-space:pre-wrap;word-break:break-all;color:#dfd}"
"</style>"
"<div class='tester'>"
"<h3>MQTT Tester <span id='conn' class='st-conn st-off'>disconnected</span></h3>"
"<fieldset><legend>&nbsp;Publish&nbsp;</legend>"
"<div class='row'><label>Topic</label><input id='ptopic' type='text' maxlength='128' placeholder='home/light/1'></div>"
"<div class='row'><label>Payload <span id='pbc' class='bc'>0/1024</span></label>"
"<textarea id='ppayload' rows='2' maxlength='1024'></textarea></div>"
"<div class='row'><label>Options</label>"
"<label class='opt'>QoS <select id='pqos' style='padding:2px'>"
"<option value='0'>0</option><option value='1'>1</option></select></label>"
"<label class='opt'><input type='checkbox' id='pretain'> retain</label>"
"<button id='pbtn' class='btn'>Publish</button>"
"<span id='perr' style='color:#f33;margin-left:8px'></span>"
"</div></fieldset>"
"<fieldset><legend>&nbsp;Subscribe (all topics)&nbsp;</legend>"
"<div class='row'><label>Filter</label>"
"<input id='filter' type='text' placeholder='MQTT filter, e.g. home/# or home/+/temp'>"
"<label class='opt'><input type='checkbox' id='showsys'> show $SYS/</label>"
"<button id='pause' class='btn'>Pause</button>"
"<button id='clear' class='btn'>Clear</button>"
"</div>"
"<div id='log'></div>"
"</fieldset>"
"<p><a href='/' class='btn'>Back</a></p>"
"</div>"
"<script>(function(){"
"var ws=null,paused=false,reconnectMs=2000,backoffMax=30000;"
"var msgs=[],MAX=200;"
"var conn=document.getElementById('conn');"
"var logEl=document.getElementById('log');"
"var filterEl=document.getElementById('filter');"
"var showsys=document.getElementById('showsys');"
"function setConn(on){conn.textContent=on?'connected':'disconnected';conn.className='st-conn '+(on?'st-on':'st-off');}"
/* Real MQTT topic-filter matching, per MQTT 3.1.1 §4.7. The previous
 * implementation lied: it advertised + and # support but stripped those
 * chars and did substring match. Now + matches exactly one level,
 * # matches the remainder. Falls back to substring when the input has no
 * wildcards/slashes so casual users keep the easy mode. */
"function topicMatch(filter,topic){if(!filter)return true;"
"var f=filter.split('/'),t=topic.split('/');"
"for(var i=0;i<f.length;i++){"
"if(f[i]==='#')return true;"
"if(i>=t.length)return false;"
"if(f[i]==='+')continue;"
"if(f[i]!==t[i])return false;}"
"return f.length===t.length;}"
"function matchFilter(t){var f=filterEl.value.trim();"
"if(!showsys.checked&&t.indexOf('$SYS/')===0)return false;"
"if(!f)return true;"
"if(f.indexOf('/')>=0||f.indexOf('+')>=0||f.indexOf('#')>=0){return topicMatch(f,t);}"
"return t.indexOf(f)>=0;}"
"function render(){logEl.innerHTML='';for(var i=msgs.length-1;i>=0;i--){var m=msgs[i];if(!matchFilter(m.t))continue;"
"var d=document.createElement('div');d.className='msg';"
"var ts=document.createElement('span');ts.className='ts';ts.textContent=m.ts+' ';d.appendChild(ts);"
"var t=document.createElement('span');t.className='t';t.textContent=m.t;d.appendChild(t);"
"if(m.r){var r=document.createElement('span');r.className='r';r.textContent=' [retained]';d.appendChild(r);}"
"if(m.trunc){var tr=document.createElement('span');tr.className='r';tr.textContent=' [truncated]';d.appendChild(tr);}"
"var p=document.createElement('pre');p.textContent=m.p;d.appendChild(p);"
"logEl.appendChild(d);}}"
"function tsNow(){var d=new Date();var p=function(n){return(n<10?'0':'')+n;};return p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());}"
"function onMsg(ev){if(paused)return;var o;try{o=JSON.parse(ev.data);}catch(e){return;}"
"if(o.hello){return;}if(o.error){var d=document.createElement('div');d.className='msg';d.style.color='#f66';d.textContent='[error] '+o.error;logEl.insertBefore(d,logEl.firstChild);return;}"
"if(typeof o.t!=='string')return;msgs.push({ts:tsNow(),t:o.t,p:o.p||'',r:o.r===1,trunc:!!o.trunc});"
"if(msgs.length>MAX)msgs=msgs.slice(msgs.length-MAX);render();}"
"function connect(){var url=(location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws';"
"try{ws=new WebSocket(url);}catch(e){setConn(false);return scheduleReconnect();}"
"ws.onopen=function(){setConn(true);reconnectMs=2000;};"
"ws.onmessage=onMsg;ws.onerror=function(){};ws.onclose=function(){setConn(false);scheduleReconnect();};}"
"function scheduleReconnect(){setTimeout(connect,reconnectMs);reconnectMs=Math.min(reconnectMs*2,backoffMax);}"
"var payEl=document.getElementById('ppayload');"
"var pbcEl=document.getElementById('pbc');"
"function updateBc(){pbcEl.textContent=payEl.value.length+'/1024';}"
"payEl.addEventListener('input',updateBc);updateBc();"
"document.getElementById('pbtn').onclick=function(){var t=document.getElementById('ptopic').value;var p=payEl.value;var r=document.getElementById('pretain').checked;var q=parseInt(document.getElementById('pqos').value,10)||0;var err=document.getElementById('perr');err.textContent='';"
"if(!t){err.textContent='topic required';return;}if(t.indexOf('#')>=0||t.indexOf('+')>=0){err.textContent='no wildcards in publish topic';return;}"
"if(!ws||ws.readyState!==1){err.textContent='not connected';return;}"
"ws.send(JSON.stringify({action:'publish',topic:t,payload:p,retain:r,qos:q}));};"
"document.getElementById('pause').onclick=function(){paused=!paused;this.textContent=paused?'Resume':'Pause';};"
"document.getElementById('clear').onclick=function(){msgs=[];render();};"
"filterEl.oninput=render;showsys.onchange=render;"
"connect();})();</script>");
        http_send_page(client_fd, body, (size_t)pos);

    /* ============ MAIN STATUS PAGE ============ */
    } else if (strcmp(req.path, "/") == 0 || strcmp(req.path, "/index.html") == 0) {
        int pos = 0;

        /* Connectivity check: prefer Ethernet, then STA WiFi, then AP-only fallback.
         * Don't show the orange "AP mode" warning when we already have a usable
         * uplink (Ethernet or STA) -- it implies the device needs setup when it
         * doesn't. AP status is reported as informational when it's the only path. */
        int sta_connected = 0, ap_running = 0;
        char ip_str[16] = "";
        char ssid_str[33] = "";
        wifi_get_ap_ip_str(ip_str, sizeof(ip_str));  /* default to AP IP */
        portal_get_sta_status(&sta_connected, &ap_running, ip_str, sizeof(ip_str),
                               ssid_str, sizeof(ssid_str));

        int eth_up = 0;
#ifdef CONFIG_MQTT_BROKER_ETHERNET
        char eth_ip[16] = "";
        eth_up = eth_is_connected();
        if (eth_up) eth_get_ip_str(eth_ip, sizeof(eth_ip));
#endif

        if (eth_up) {
#ifdef CONFIG_MQTT_BROKER_ETHERNET
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<div class='st ok'>Online \xc2\xb7 Ethernet %s (%s)</div>",
                eth_ip, eth_napt_is_enabled() ? "NAPT on" : "NAPT off");
#endif
            if (sta_connected) {
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<div class='st ok'>WiFi \xc2\xb7 %s (%s)</div>", ssid_str, ip_str);
            }
        } else if (sta_connected) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<div class='st ok'>Online \xc2\xb7 WiFi %s (%s)</div>", ssid_str, ip_str);
        } else {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<div class='st warn'>Setup \xc2\xb7 AP mode @ %s</div>", ip_str);
        }

        /* Broker quick stats */
        broker_stats_t stats;
        broker_get_stats(&stats);
        int uptime_sec = (int)(stats.uptime_ms / 1000);
        int up_h = uptime_sec / 3600;
        int up_m = (uptime_sec % 3600) / 60;

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Broker&nbsp;</legend><table>"
            "<tr><th>Clients</th><td>%d / %d</td></tr>"
            "<tr><th>Subscriptions</th><td>%d</td></tr>"
            "<tr><th>Uptime</th><td>%dh %dm</td></tr>"
            "</table></fieldset>",
            stats.connected_clients, BROKER_MAX_CLIENTS,
            stats.active_subscriptions,
            up_h, up_m);

        /* Grouped navigation — labelled sections, Tasmota-style full-width buttons */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<style>"
            ".mgrp{margin:10px 0 4px;font-size:0.72em;letter-spacing:0.08em;"
            "text-transform:uppercase;color:#888;padding-left:2px}"
            "</style>"
            "<br>"
            /* Network */
            "<p class='mgrp'>Network</p>"
            "<a href='/config' class='btn'>Configure WiFi</a>"
            "<a href='/time' class='btn'>Time / NTP</a>"
            /* Broker */
            "<p class='mgrp'>Broker</p>"
            "<a href='/clients' class='btn'>Connected Clients</a>"
            "<a href='/tester' class='btn'>MQTT Tester</a>"
            "<a href='/timers' class='btn'>Timers</a>"
            "<a href='/berry' class='btn'>Berry Scripting</a>"
            /* System */
            "<p class='mgrp'>System</p>"
            "<a href='/settings' class='btn'>Configuration</a>"
            "<a href='/information' class='btn'>Information</a>"
            "<a href='/update' class='btn bgry'>Firmware Upgrade</a>"
            "<form method='POST' action='/reboot'"
            " style='display:inline-block;width:100%%;margin:0;padding:0' "
            "onsubmit=\"return confirm('Restart?')\">"
            "<input type='hidden' name='csrf' value='%s'>"
            "<button type='submit' class='btn bred'>Restart</button>"
            "</form>",
            csrf_token_hex());

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ INFORMATION PAGE ============ */
    } else if (strcmp(req.path, "/information") == 0) {
        int pos = 0;

        /* WiFi */
        int sta_connected = 0, ap_running = 0;
        char ip_str[16] = "";
        char ssid_str[33] = "";
        wifi_get_ap_ip_str(ip_str, sizeof(ip_str));  /* default to AP IP */
        portal_get_sta_status(&sta_connected, &ap_running, ip_str, sizeof(ip_str),
                              ssid_str, sizeof(ssid_str));

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;WiFi&nbsp;</legend><table>");
        if (sta_connected) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<tr><th>SSID</th><td>%s</td></tr>"
                "<tr><th>IP Address</th><td>%s</td></tr>"
                "<tr><th>AP Mode</th><td>%s</td></tr>",
                ssid_str, ip_str, ap_running ? "Enabled" : "Disabled");
        } else {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<tr><th>Status</th><td>Not connected</td></tr>"
                "<tr><th>AP IP</th><td>%s</td></tr>", ip_str);
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</table></fieldset>");

        /* Ethernet (only when compiled with W5500 support) */
#ifdef CONFIG_MQTT_BROKER_ETHERNET
        {
            char eth_ip[16] = "";
            int eth_up = eth_is_connected();
            if (eth_up) eth_get_ip_str(eth_ip, sizeof(eth_ip));

            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<fieldset><legend>&nbsp;Ethernet&nbsp;</legend><table>"
                "<tr><th>Status</th><td>%s</td></tr>",
                eth_up ? "Connected" : "Disconnected");
            if (eth_up) {
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<tr><th>IP Address</th><td>%s</td></tr>"
                    "<tr><th>NAPT</th><td>%s</td></tr>",
                    eth_ip, eth_napt_is_enabled() ? "Enabled" : "Disabled");
            }
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</table></fieldset>");
        }
#endif

        /* Broker stats */
        broker_stats_t stats;
        broker_get_stats(&stats);

        int uptime_sec = (int)(stats.uptime_ms / 1000);
        int up_h = uptime_sec / 3600;
        int up_m = (uptime_sec % 3600) / 60;
        int up_s = uptime_sec % 60;

        char ttl_str[32];
        if (!stats.retain_enabled) {
            snprintf(ttl_str, sizeof(ttl_str), "N/A (disabled)");
        } else if (stats.retain_ttl_sec <= 0) {
            snprintf(ttl_str, sizeof(ttl_str), "Never expire");
        } else {
            int ttl_h = (int)(stats.retain_ttl_sec / 3600);
            snprintf(ttl_str, sizeof(ttl_str), "%d hours", ttl_h);
        }

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;MQTT Broker&nbsp;</legend><table>"
            "<tr><th>MQTT Port</th><td>%d</td></tr>"
            "<tr><th>Connected Clients</th><td>%d / %d</td></tr>"
            "<tr><th>Subscriptions</th><td>%d / %d</td></tr>"
            "<tr><th>Retained Messages</th><td>%lu (%lu KB)</td></tr>"
            "<tr><th>Retain</th><td>%s</td></tr>"
            "<tr><th>Retain TTL</th><td>%s</td></tr>"
            "<tr><th>Buffer Size</th><td>%u KB</td></tr>"
            "<tr><th>Uptime</th><td>%dh %dm %ds</td></tr>"
            "<tr><th>Free Heap</th><td>%lu KB</td></tr>"
            "</table></fieldset>",
            BROKER_PORT,
            stats.connected_clients, BROKER_MAX_CLIENTS,
            stats.active_subscriptions, BROKER_MAX_SUBSCRIPTIONS,
            (unsigned long)stats.retained_count, (unsigned long)(stats.retained_bytes / 1024),
            stats.retain_enabled ? "Enabled" : "Disabled",
            ttl_str,
            stats.buf_size / 1024,
            up_h, up_m, up_s,
            (unsigned long)(stats.free_heap / 1024));

        /* MQTT auth status */
        char auth_user[65] = "";
        nvs_settings_get_str("auth_user", auth_user, sizeof(auth_user), "");
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;MQTT Authentication&nbsp;</legend><table>"
            "<tr><th>Status</th><td>%s</td></tr>",
            auth_user[0] ? "Enabled" : "Disabled (open)");
        if (auth_user[0]) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<tr><th>Username</th><td>%s</td></tr>", auth_user);
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</table></fieldset>");

        /* AP config.
         *
         * Password is NEVER rendered in plaintext on the information page or
         * sent inside the HTML source. The portal previously embedded
         * `mqtt1234` (or the user-set value) in the response body, which any
         * device on the same LAN could read with a single GET. Now we only
         * indicate whether the AP is using its factory-default password so
         * users know whether they need to change it. The actual value is only
         * accessible after explicit POST via /settings (see Phase 4 plan). */
        char ap_ssid[33] = "mqtt-broker";
        char ap_pass[65] = "mqtt1234";
        char ap_ip_info[16] = "";
        nvs_settings_get_str("ap_ssid", ap_ssid, sizeof(ap_ssid), "mqtt-broker");
        nvs_settings_get_str("ap_pass", ap_pass, sizeof(ap_pass), "mqtt1234");
        wifi_get_ap_ip_str(ap_ip_info, sizeof(ap_ip_info));
        int ap_pass_is_default = (strcmp(ap_pass, "mqtt1234") == 0);
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Access Point&nbsp;</legend><table>"
            "<tr><th>AP SSID</th><td>%s</td></tr>"
            "<tr><th>AP Password</th><td>%s</td></tr>"
            "<tr><th>AP IP</th><td>%s</td></tr>"
            "</table></fieldset>",
            ap_ssid,
            ap_pass_is_default
                ? "<span style='color:#ffcc80'>set (factory default \xe2\x80\x94 change in Configuration)</span>"
                : "<span style='color:#a5d6a7'>set (custom)</span>",
            ap_ip_info);

        /* Device / firmware info */
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        uint32_t flash_size = 0;
        esp_flash_get_size(NULL, &flash_size);
        const esp_app_desc_t *app = esp_app_get_description();
        const esp_partition_t *running = esp_ota_get_running_partition();

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Device&nbsp;</legend><table>"
            "<tr><th>Firmware</th><td>" FW_FOOTER "</td></tr>"
            "<tr><th>Build</th><td>" __DATE__ " " __TIME__ "</td></tr>"
            "<tr><th>IDF Version</th><td>%s</td></tr>"
            "<tr><th>Partition</th><td>%s</td></tr>"
            "<tr><th>Chip</th><td>ESP32-S3 rev %d.%d (%d cores)</td></tr>"
            "<tr><th>MAC</th><td>%02X:%02X:%02X:%02X:%02X:%02X</td></tr>"
            "<tr><th>PSRAM</th><td>%lu KB</td></tr>"
            "<tr><th>Flash</th><td>%lu KB</td></tr>"
            "</table></fieldset>",
            app->idf_ver,
            running ? running->label : "unknown",
            chip.revision / 100, chip.revision % 100, chip.cores,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned long)(flash_size / 1024));

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<br><a href='/' class='btn'>Main Menu</a>");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ SETTINGS PAGE ============ */
    } else if (strcmp(req.path, "/settings") == 0) {
        int pos = 0;

        /* Load current settings.
         *
         * IMPORTANT: passwords (auth_pass, ap_pass) are intentionally never
         * echoed back into the form HTML. The previous version put the actual
         * value into <input value='...'>, which made it readable via View
         * Source on the page. Instead the inputs render empty with a
         * placeholder "unchanged \xe2\x80\x94 leave blank to keep" and the
         * /save-settings handler treats an empty submission as "keep current".
         * We DO still load auth_pass / ap_pass so we can show whether they are
         * currently set, but we never put the value into the response. */
        char auth_user[65] = "";
        char auth_pass[65] = "";
        char ap_ssid[33] = "mqtt-broker";
        char ap_pass[65] = "mqtt1234";
        uint16_t mqtt_port = nvs_settings_get_u16("mqtt_port", BROKER_PORT);
        uint16_t buf_size = nvs_settings_get_u16("buf_size", BROKER_RECV_BUF_SIZE_DEFAULT);
        nvs_settings_get_str("auth_user", auth_user, sizeof(auth_user), "");
        nvs_settings_get_str("auth_pass", auth_pass, sizeof(auth_pass), "");
        nvs_settings_get_str("ap_ssid", ap_ssid, sizeof(ap_ssid), "mqtt-broker");
        nvs_settings_get_str("ap_pass", ap_pass, sizeof(ap_pass), "mqtt1234");
        char hostname[33] = "";
        portal_get_hostname(hostname, sizeof(hostname));
        int auth_pass_set = (auth_pass[0] != '\0');
        int ap_pass_is_default = (strcmp(ap_pass, "mqtt1234") == 0);
        /* Wipe the secret values out of the local buffers as soon as we're
         * done deciding what to render -- belt-and-braces against the
         * compiler re-using these stack slots later in the same function. */
        memset(auth_pass, 0, sizeof(auth_pass));
        memset(ap_pass, 0, sizeof(ap_pass));

        /* Device / network identity */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Device&nbsp;</legend>"
            "<form method='POST' action='/save-settings'>"
            /* CSRF token: hidden input, validated by csrf_verify() in
             * the /save-settings POST handler. */
            "<input type='hidden' name='csrf' value='%s'>"
            "<label>Hostname (requires reboot)</label>"
            "<input type='text' name='hostname' value='%s' "
            "pattern='[A-Za-z0-9_\\-]{1,32}' maxlength='32' required "
            "title='1-32 chars: letters, digits, hyphen, underscore'>"
            "</fieldset>",
            csrf_token_hex(), hostname);

        /* MQTT settings.
         * Auth password input is rendered empty -- the /save-settings handler
         * treats an empty `auth_pass` field as "keep current value". To clear
         * the password, also clear `auth_user` (which disables auth entirely),
         * or POST `auth_pass_clear=1`. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;MQTT Broker&nbsp;</legend>"
            "<label>MQTT Port</label>"
            "<input type='number' name='mqtt_port' value='%u' min='1' max='65535'>"
            "<label>Auth Username (blank = disabled)</label>"
            "<input type='text' name='auth_user' value='%s' placeholder='Leave empty to disable'>"
            "<label>Auth Password %s</label>"
            "<input type='password' name='auth_pass' value='' placeholder='%s' autocomplete='new-password'>"
            "<label>Buffer Size (bytes, recv and send)</label>"
            "<input type='number' name='buf_size' value='%u' min='1024' max='65536' step='1024'>",
            mqtt_port, auth_user,
            auth_pass_set ? "(currently set)" : "(not set)",
            auth_pass_set ? "unchanged \xe2\x80\x94 leave blank to keep" : "Password",
            buf_size);

        /* Retained message settings */
        uint8_t retain_en = nvs_settings_get_u8("retain_en", 1);
        int32_t retain_ttl = nvs_settings_get_i32("retain_ttl", BROKER_RETAIN_TTL_SEC_DEFAULT);
        int retain_ttl_hours = (int)(retain_ttl / 3600);

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "</fieldset>"
            "<fieldset><legend>&nbsp;Retained Messages&nbsp;</legend>"
            "<p><label style='font-weight:normal'>"
            "<input type='checkbox' name='retain_en' value='1' %s> "
            "Enable retained messages</label></p>"
            "<label>Retain TTL (hours, 0 = never expire)</label>"
            "<input type='number' name='retain_ttl_h' value='%d' min='0' max='8760'>",
            retain_en ? "checked" : "", retain_ttl_hours);

        /* AP settings in same form.
         * AP password is rendered empty (not echoed back). Empty submission
         * keeps the existing value -- so the `required` constraint is removed
         * to make saving other AP fields possible without re-typing the
         * password every time. minlength still enforces 8 chars when the user
         * does choose to change it. */
        {
            char ap_ip_val[16] = "";
            wifi_get_ap_ip_str(ap_ip_val, sizeof(ap_ip_val));
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "</fieldset>"
                "<fieldset><legend>&nbsp;Access Point&nbsp;</legend>"
                "<label>AP SSID</label>"
                "<input type='text' name='ap_ssid' value='%s' placeholder='mqtt-broker' required maxlength='32'>"
                "<label>AP Password %s</label>"
                "<input type='password' name='ap_pass' value='' placeholder='%s' "
                "minlength='8' maxlength='63' "
                "title='AP password must be 8-63 characters' autocomplete='new-password'>"
                "<label>AP IP Address (requires reboot)</label>"
                "<input type='text' name='ap_ip' value='%s' "
                "pattern='[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+' "
                "title='IPv4 address like 192.168.25.1'>",
                ap_ssid,
                ap_pass_is_default
                    ? "<span style='color:#ffcc80'>(factory default \xe2\x80\x94 please change)</span>"
                    : "(currently set)",
                "unchanged \xe2\x80\x94 leave blank to keep",
                ap_ip_val);
        }

#ifdef CONFIG_MQTT_BROKER_ETHERNET
        /* NAPT setting — only when Ethernet support is compiled in */
        {
            uint8_t napt_en = nvs_settings_get_u8("napt_en", 1);
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "</fieldset>"
                "<fieldset><legend>&nbsp;Ethernet Gateway&nbsp;</legend>"
                "<p><label style='font-weight:normal'>"
                "<input type='checkbox' name='napt_en' value='1' %s> "
                "Enable NAPT (LAN access to WiFi AP devices)</label></p>"
                "<p style='color:#888;font-size:0.85em'>"
                "When enabled, devices on the Ethernet LAN can reach WiFi AP clients "
                "(e.g., Tasmota web UIs on the AP subnet). Disable for full network isolation. "
                "Takes effect immediately.</p>",
                napt_en ? "checked" : "");
        }
#endif

        /* Time (NTP) -- Phase 1 of plan-ntp-server.md. Renders the current
         * sync state (read-only) and the editable upstreams + poll + tz.
         * State pulled live via ntp_get_state() so the form reflects the
         * latest sync; settings pulled from NVS so empty inputs map to
         * sensible defaults. */
        {
            ntp_settings_t ns;
            ntp_get_settings(&ns);
            ntp_state_t nstate;
            ntp_get_state(&nstate);
            /* Server-enable mirror -- ntp_get_settings() doesn't expose
             * srv_enabled today; read it directly. Default ON per plan. */
            uint8_t srv_en = 1;
            uint8_t accept_set = 0;  /* Phase 4: manual POST /api/time/set */
            {
                nvs_handle_t snh;
                if (nvs_open("ntp", NVS_READONLY, &snh) == ESP_OK) {
                    nvs_get_u8(snh, "srv_enabled", &srv_en);
                    nvs_get_u8(snh, "accept_set", &accept_set);
                    nvs_close(snh);
                }
            }
            char sync_summary[96];
            if (nstate.synced) {
                snprintf(sync_summary, sizeof(sync_summary),
                         "<span style='color:#a5d6a7'>synced</span> \xc2\xb7 "
                         "last %llds ago \xc2\xb7 %u total",
                         (long long)nstate.last_sync_age_s,
                         (unsigned)nstate.sync_count);
            } else {
                snprintf(sync_summary, sizeof(sync_summary),
                         "<span style='color:#ffcc80'>not yet synced</span>");
            }
            char server_summary[160];
            if (nstate.server_running) {
                snprintf(server_summary, sizeof(server_summary),
                         "<span style='color:#a5d6a7'>serving</span> on UDP:123 \xc2\xb7 "
                         "stratum %u \xc2\xb7 %u served \xc2\xb7 dropped %u/%u/%u (rate/size/mode)",
                         (unsigned)nstate.stratum,
                         (unsigned)nstate.served,
                         (unsigned)nstate.dropped_rate,
                         (unsigned)nstate.dropped_size,
                         (unsigned)nstate.dropped_mode);
            } else {
                snprintf(server_summary, sizeof(server_summary),
                         "<span style='color:#888'>server off</span>");
            }
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "</fieldset>"
                "<fieldset><legend>&nbsp;Time (NTP)&nbsp;</legend>"
                "<p style='color:#aaa;font-size:0.85em;margin:0'>client \xc2\xb7 %s</p>"
                "<p style='color:#aaa;font-size:0.85em;margin:0 0 6px 0'>server \xc2\xb7 %s</p>"
                "<p><label style='font-weight:normal'>"
                "<input type='checkbox' name='ntp_en' value='1' %s> "
                "Enable SNTP client</label></p>"
                "<p><label style='font-weight:normal'>"
                "<input type='checkbox' name='ntp_srv_en' value='1' %s> "
                "Enable SNTP server (UDP:123) \xe2\x80\x94 LAN clients can sync from this device</label></p>"
                "<label>Upstream 1</label>"
                "<input type='text' name='ntp_up0' value='%s' maxlength='63' placeholder='pool.ntp.org'>"
                "<label>Upstream 2 (optional)</label>"
                "<input type='text' name='ntp_up1' value='%s' maxlength='63' placeholder='time.cloudflare.com'>"
                "<label>Upstream 3 (optional)</label>"
                "<input type='text' name='ntp_up2' value='%s' maxlength='63'>"
                "<label>Poll interval (seconds, 64-86400)</label>"
                "<input type='number' name='ntp_poll' value='%u' min='64' max='86400'>"
                /* TZ field: a <select> dropdown of common presets + the
                 * underlying POSIX TZ text input. Picking a preset copies
                 * its value into the text field via a tiny inline handler;
                 * the text field remains the single source of truth that
                 * POSTs and persists. Users with exotic zones still hand-
                 * type. Presets table lives in main/tz_presets.c. */
                "<label>Timezone</label>"
                "<select id='tz_preset' style='width:100%%;box-sizing:border-box;"
                "background:#1f1f1f;color:#eaeaea;border:1px solid #555;"
                "border-radius:0.3rem;padding:8px'>"
                "<option value=''>— Pick a preset, or hand-type below —</option>",
                sync_summary, server_summary,
                ns.enabled ? "checked" : "",
                srv_en ? "checked" : "",
                ns.upstreams[0], ns.upstreams[1], ns.upstreams[2],
                (unsigned)ns.poll_s);
            for (size_t k = 0; k < TZ_PRESETS_COUNT; k++) {
                bool selected = (strcmp(TZ_PRESETS[k].posix, ns.tz) == 0);
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<option value='%s'%s>%s</option>",
                    TZ_PRESETS[k].posix,
                    selected ? " selected" : "",
                    TZ_PRESETS[k].label);
            }
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "</select>"
                "<input type='text' id='ntp_tz' name='ntp_tz' value='%s' "
                "maxlength='63' placeholder='UTC0' "
                "style='margin-top:6px;font-family:monospace'>"
                "<p style='color:#888;font-size:0.8em;margin:2px 0 0'>"
                "POSIX TZ string. Picking a preset above fills this field."
                " Examples: <code>UTC0</code>, <code>PST8PDT,M3.2.0,M11.1.0</code>,"
                " <code>&lt;+0530&gt;-5:30</code> (India).</p>"
                /* Wire dropdown -> text field. Plain JS so it works
                 * without any framework; the change handler copies the
                 * selected POSIX string into the text input the form
                 * actually submits. If JS is disabled the dropdown is
                 * inert but the text field still works. */
                "<script>"
                "(function(){var s=document.getElementById('tz_preset'),"
                "t=document.getElementById('ntp_tz');"
                "if(!s||!t)return;"
                "s.addEventListener('change',function(){if(s.value)t.value=s.value;});"
                "})();"
                "</script>"
                /* Phase 4 of plan-ntp-server.md: opt-in manual time set.
                 * Off by default. Wording is deliberately blunt --
                 * operators who don't need it should never see the form
                 * on /time, and operators who do need it should
                 * understand the trust model they're stepping into. */
                "<p style='margin-top:10px'><label style='font-weight:normal'>"
                "<input type='checkbox' name='ntp_accept_set' value='1' %s> "
                "Accept manual time set (POST /api/time/set)</label></p>"
                "<p style='color:#ffcc80;font-size:0.8em;margin:-4px 0 0 24px'>"
                "For air-gapped installs only. When on, an authenticated user "
                "can set the clock from /time while no upstream is reachable. "
                "Any real upstream sync immediately supersedes the manual value."
                "</p>",
                ns.tz,
                accept_set ? "checked" : "");
        }

        /* The Save button triggers a reboot so changes apply uniformly
         * (MQTT port, auth credentials, retained-message toggle, NAPT, AP
         * config -- the previous "some take effect immediately, some need a
         * reboot" split was a source of confusion). Confirm runs before the
         * POST is dispatched, so cancelling really cancels: nothing is
         * written to NVS. After confirmation the /save-settings handler
         * persists every field, then serves the reboot-countdown page and
         * restarts the device. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "</fieldset>"
            "<p style='color:#aaa;font-size:0.85em;margin-top:8px'>"
            "Saving will reboot the device so all changes take effect uniformly. "
            "Reconnect in about 10 seconds.</p>"
            "<button type='submit' class='bgrn' "
            "onclick=\"return confirm('Save settings and reboot? The device will "
            "restart and should be reachable again in about 10 seconds.')\">"
            "Save &amp; Reboot</button>"
            "</form>"
            "<br><a href='/' class='btn'>Main Menu</a>");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ SAVE SETTINGS ============
     * Persists every field, then serves the reboot-countdown page and
     * restarts the device. The previous flow redirected to /settings with
     * no feedback, requiring users to manually find the Restart button --
     * and not every setting was honored without a reboot anyway (MQTT port,
     * buffer size, retained-message toggle, AP password all needed one).
     * Unified to "always reboot" matches user expectation. */
    } else if (strcmp(req.path, "/save-settings") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        char val[65];

        if (urldecode_param(req.body, "hostname", val, sizeof(val))) {
            if (hostname_is_valid(val)) {
                nvs_settings_set_str("hostname", val);
                ESP_LOGI(TAG, "Saved hostname: '%s' (reboot required)", val);
            } else {
                ESP_LOGW(TAG, "Hostname rejected: invalid '%s'", val);
            }
        }
        if (urldecode_param(req.body, "mqtt_port", val, sizeof(val))) {
            int port = atoi(val);
            if (port > 0 && port <= 65535) {
                nvs_settings_set_u16("mqtt_port", (uint16_t)port);
                ESP_LOGI(TAG, "Saved MQTT port: %d", port);
            }
        }
        if (urldecode_param(req.body, "buf_size", val, sizeof(val))) {
            int sz = atoi(val);
            if (sz >= 1024 && sz <= 65536) {
                nvs_settings_set_u16("buf_size", (uint16_t)sz);
                ESP_LOGI(TAG, "Saved buffer size: %d", sz);
            }
        }
        if (urldecode_param(req.body, "auth_user", val, sizeof(val))) {
            nvs_settings_set_str("auth_user", val);
            ESP_LOGI(TAG, "Saved auth user: '%s'", val);
        }
        if (urldecode_param(req.body, "auth_pass", val, sizeof(val))) {
            /* Empty value means "keep current password". Use auth_pass_clear=1
             * (or clear auth_user) to actually remove a stored password. */
            if (val[0] != '\0') {
                nvs_settings_set_str("auth_pass", val);
                ESP_LOGI(TAG, "Saved auth pass (len=%d)", (int)strlen(val));
            } else if (strstr(req.body, "auth_pass_clear=1")) {
                nvs_settings_set_str("auth_pass", "");
                ESP_LOGI(TAG, "Cleared auth pass on explicit request");
            } else {
                ESP_LOGI(TAG, "Auth pass unchanged (empty submission)");
            }
        }
        if (urldecode_param(req.body, "ap_ssid", val, sizeof(val))) {
            if (val[0]) {
                nvs_settings_set_str("ap_ssid", val);
                ESP_LOGI(TAG, "Saved AP SSID: '%s'", val);
            }
        }
        if (urldecode_param(req.body, "ap_pass", val, sizeof(val))) {
            /* Empty value means "keep current AP password" -- the form input is
             * rendered without value= so we don't leak the stored value. A
             * non-empty value must still satisfy the 8-char minimum. */
            if (val[0] == '\0') {
                ESP_LOGI(TAG, "AP pass unchanged (empty submission)");
            } else if (strlen(val) >= 8) {
                nvs_settings_set_str("ap_pass", val);
                ESP_LOGI(TAG, "Saved AP pass (len=%d)", (int)strlen(val));
            } else {
                ESP_LOGW(TAG, "AP password rejected: too short (%d chars)", (int)strlen(val));
            }
        }
        if (urldecode_param(req.body, "ap_ip", val, sizeof(val))) {
            /* Basic validation: must have 4 octets */
            unsigned int a, b, c, d;
            if (sscanf(val, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
                a <= 255 && b <= 255 && c <= 255 && d <= 255) {
                nvs_settings_set_str("ap_ip", val);
                ESP_LOGI(TAG, "Saved AP IP: '%s' (reboot required)", val);
            } else {
                ESP_LOGW(TAG, "AP IP rejected: invalid format '%s'", val);
            }
        }

        /* Retain settings — checkbox absent means unchecked (disabled) */
        {
            uint8_t retain_en = (strstr(req.body, "retain_en=1") != NULL) ? 1 : 0;
            nvs_settings_set_u8("retain_en", retain_en);
            ESP_LOGI(TAG, "Saved retain enabled: %d", retain_en);
        }
        if (urldecode_param(req.body, "retain_ttl_h", val, sizeof(val))) {
            int hours = atoi(val);
            if (hours >= 0 && hours <= 8760) {
                int32_t ttl_sec = (int32_t)hours * 3600;
                nvs_settings_set_i32("retain_ttl", ttl_sec);
                ESP_LOGI(TAG, "Saved retain TTL: %d hours (%ld sec)", hours, (long)ttl_sec);
            }
        }

#ifdef CONFIG_MQTT_BROKER_ETHERNET
        /* NAPT toggle -- checkbox absent means unchecked (disabled).
         * NVS-only here; the eth subsystem reads NVS on boot and applies
         * the desired state. (Previously this also called eth_napt_enable()
         * / eth_napt_disable() to take effect immediately, but with the
         * unified "save = reboot" flow that's redundant -- the reboot below
         * re-applies it cleanly from NVS.) */
        {
            uint8_t napt_en = (strstr(req.body, "napt_en=1") != NULL) ? 1 : 0;
            nvs_settings_set_u8("napt_en", napt_en);
            ESP_LOGI(TAG, "Saved NAPT enabled: %d", napt_en);
        }
#endif

        /* NTP settings -- separate "ntp" NVS namespace per the plan. Done
         * inline here (rather than adding portal-wide helpers) because
         * we open the namespace once, write all six keys, commit, and
         * close. ntp_init() reads these on the next boot. */
        {
            nvs_handle_t nh;
            if (nvs_open("ntp", NVS_READWRITE, &nh) == ESP_OK) {
                uint8_t ntp_en = (strstr(req.body, "ntp_en=1") != NULL) ? 1 : 0;
                nvs_set_u8(nh, "enabled", ntp_en);
                ESP_LOGI(TAG, "Saved NTP enabled: %d", ntp_en);

                uint8_t srv_en = (strstr(req.body, "ntp_srv_en=1") != NULL) ? 1 : 0;
                nvs_set_u8(nh, "srv_enabled", srv_en);
                ESP_LOGI(TAG, "Saved NTP server enabled: %d", srv_en);

                if (urldecode_param(req.body, "ntp_up0", val, sizeof(val))) {
                    nvs_set_str(nh, "upstream_0", val);
                }
                if (urldecode_param(req.body, "ntp_up1", val, sizeof(val))) {
                    nvs_set_str(nh, "upstream_1", val);
                }
                if (urldecode_param(req.body, "ntp_up2", val, sizeof(val))) {
                    nvs_set_str(nh, "upstream_2", val);
                }
                if (urldecode_param(req.body, "ntp_poll", val, sizeof(val))) {
                    long poll = strtol(val, NULL, 10);
                    if (poll >= 64 && poll <= 86400) {
                        nvs_set_u32(nh, "poll_s", (uint32_t)poll);
                    } else {
                        ESP_LOGW(TAG, "NTP poll_s out of range: %ld", poll);
                    }
                }
                if (urldecode_param(req.body, "ntp_tz", val, sizeof(val))) {
                    nvs_set_str(nh, "tz", val[0] ? val : "UTC0");
                }
                /* Phase 4: accept_set toggle. Default OFF per the plan;
                 * unchecked checkboxes don't appear in form bodies so
                 * absence == 0. */
                uint8_t accept_set = (strstr(req.body, "ntp_accept_set=1") != NULL) ? 1 : 0;
                nvs_set_u8(nh, "accept_set", accept_set);
                ESP_LOGI(TAG, "Saved NTP accept_set: %d", accept_set);
                nvs_commit(nh);
                nvs_close(nh);
            } else {
                ESP_LOGW(TAG, "Could not open NVS namespace 'ntp' to save");
            }
        }

        ESP_LOGW(TAG, "Settings saved; rebooting on user request");
        /* Subtitle wording per user request: surface that we've saved AND
         * that the countdown page is actively polling, with a clear promise
         * of a redirect home -- no more "reconnect in 10s" guesswork. */
        http_send_reboot_countdown(client_fd,
            "Saving and rebooting",
            "Saved. Polling device \xe2\x80\x94 will redirect home when it comes back online.",
            "/");
        free(body);
        close(client_fd);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;  /* not reached */

    /* ============ WIFI CONFIG PAGE ============ */
    } else if (strcmp(req.path, "/config") == 0) {
        int pos = 0;

        /* Same "save = reboot" pattern as /settings -- the WiFi stack picks
         * up new STA credentials cleanly on boot, and a reboot is the
         * simplest way to guarantee a fresh association attempt. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;WiFi Configuration&nbsp;</legend>"
            "<form method='POST' action='/save'>"
            "<input type='hidden' name='csrf' value='%s'>"
            "<label>WiFi SSID</label>"
            "<input type='text' name='ssid' placeholder='Network name' required maxlength='32' autofocus>"
            "<label>WiFi Password</label>"
            "<input type='password' name='password' placeholder='Network password' "
            "minlength='8' maxlength='63' required "
            "title='WiFi password must be 8-63 characters'>"
            "<p><label style='font-weight:normal'>"
            "<input type='checkbox' name='ap_mode' value='1' %s> "
            "Enable AP mode alongside WiFi</label></p>"
            "<p style='color:#aaa;font-size:0.85em;margin-top:8px'>"
            "Saving will reboot the device to apply the new WiFi credentials. "
            "Reconnect in about 10 seconds.</p>"
            "<button type='submit' class='bgrn' "
            "onclick=\"return confirm('Save WiFi credentials and reboot? The device will "
            "restart and try to associate with the new network.')\">"
            "Save &amp; Reboot</button>"
            "</form></fieldset>"
            "<br><a href='/' class='btn'>Main Menu</a>",
            csrf_token_hex(),
            s_portal_ap_mode ? "checked" : "");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ SAVE WIFI ============ */
    } else if (strcmp(req.path, "/save") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        char ssid[33] = "", password[65] = "";
        int ap_mode = 0;
        urldecode_param(req.body, "ssid", ssid, sizeof(ssid));
        urldecode_param(req.body, "password", password, sizeof(password));
        if (strstr(req.body, "ap_mode=1")) ap_mode = 1;

        if (ssid[0] == '\0' || strlen(password) < 8) {
            ESP_LOGW(TAG, "WiFi save rejected: SSID empty or password too short");
            http_send_redirect(client_fd, "/config");
        } else {
            ESP_LOGI(TAG, "Saving WiFi: SSID='%s', ap_mode=%d (rebooting)", ssid, ap_mode);
            portal_save_wifi(ssid, password, ap_mode);
            /* Match the /save-settings flow: reboot so the WiFi stack comes
             * up fresh with the new credentials. The captive portal first-
             * boot user is on the AP -- after the device reboots they may
             * lose AP connectivity if STA succeeds, so the countdown page's
             * back-online detection may never fire on their phone. They
             * still get a clean "Device offline, will resume when it
             * returns..." indicator and the meta-refresh fallback. */
            char subtitle[128];
            snprintf(subtitle, sizeof(subtitle),
                     "Saved credentials for %.32s. Reconnecting...", ssid);
            http_send_reboot_countdown(client_fd,
                "Saving and rebooting", subtitle, "/");
            free(body);
            close(client_fd);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            return;  /* not reached */
        }

    /* ============ CLEAR WIFI ============ */
    } else if (strcmp(req.path, "/clear") == 0) {
        ESP_LOGI(TAG, "Clearing WiFi credentials");
        portal_clear_wifi();
        http_send_redirect(client_fd, "/");

    /* ============ AP TOGGLE ============ */
    } else if (strcmp(req.path, "/ap-toggle") == 0) {
        int enabled = 0;
        if (portal_get_ap_enabled(&enabled) == 0) {
            enabled = !enabled;
            portal_set_ap_enabled(enabled);
            ESP_LOGI(TAG, "AP mode %s", enabled ? "enabled" : "disabled");
        }
        http_send_redirect(client_fd, "/");

    /* ============ RECONNECT ============ */
    } else if (strcmp(req.path, "/reconnect") == 0) {
        ESP_LOGI(TAG, "Reconnecting WiFi");
        portal_reconnect_wifi();
        vTaskDelay(pdMS_TO_TICKS(2000));
        http_send_redirect(client_fd, "/");

    /* ============ QUICK CONNECT ============ */
    } else if (strcmp(req.path, "/quickconnect") == 0) {
        ESP_LOGI(TAG, "Quick reconnect");
        portal_reconnect_wifi();
        http_send_redirect(client_fd, "/");

    /* ============ REBOOTING VIEW ============
     * Read-only view of the reboot countdown page. Does NOT trigger a reboot;
     * just shows the polling UI so OTA flows can redirect to it immediately
     * once they know the device is going down. If the device has actually
     * already come back when the user lands here, /api/status will succeed,
     * we'll never observe the offline edge, and the page will stay in
     * 'Device still up, waiting for reboot...' until the user navigates away. */
    } else if (strcmp(req.path, "/rebooting") == 0) {
        http_send_reboot_countdown(client_fd,
            "Device rebooting",
            "Waiting for the device to come back online.",
            "/");
        /* Fall through to normal cleanup -- no esp_restart here. */

    /* ============ REBOOT ============
     * Promoted from GET to POST in the CSRF pass so it can't be
     * triggered by an attacker's <img src> tag. Dashboard "Restart"
     * button now wraps the action in a <form method=POST> with the
     * hidden CSRF token. */
    } else if (strcmp(req.path, "/reboot") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body);
            close(client_fd);
            return;
        }
        /* Reboot countdown page (shared helper). Polls /api/status every 1s
         * and replaces itself with a green 'Back online' link as soon as the
         * device returns. Previously this dumped the user on a broken page
         * with no indication of when it would be safe to retry. */
        http_send_reboot_countdown(client_fd,
            "Rebooting",
            "The device is restarting. This usually takes 5-10 seconds.",
            "/");
        free(body);
        close(client_fd);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;  /* not reached */

    /* ============ CONNECTED CLIENTS PAGE ============ */
    } else if (strcmp(req.path, "/clients") == 0) {
        int pos = 0;

        /* MQTT broker clients */
        broker_client_info_t *clients = (broker_client_info_t *)malloc(
            BROKER_MAX_CLIENTS * sizeof(broker_client_info_t));
        int mqtt_count = 0;
        if (clients) {
            mqtt_count = broker_get_clients(clients, BROKER_MAX_CLIENTS);

            /* Sort numerically by last octet of IP (a.b.c.X). Unparseable
             * IPs sort to 0. Comparator is at file scope (see above). */
            if (mqtt_count > 1) {
                qsort(clients, mqtt_count, sizeof(broker_client_info_t),
                      cmp_client_by_last_octet);
            }
        }

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;MQTT Clients (%d)&nbsp;</legend>", mqtt_count);

        if (mqtt_count == 0) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<p style='color:#888;text-align:center;padding:10px'>No MQTT clients connected</p>");
        } else {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<table><tr>"
                "<th style='width:20%%'>Client ID</th>"
                "<th style='width:16%%'>IP Address</th>"
                "<th style='width:12%%'>Connected</th>"
                "<th style='width:12%%'>Last Active</th>"
                "<th style='width:7%%' title='Active subscriptions'>Subs</th>"
                "<th style='width:8%%' title='Outbound QoS-1 messages awaiting PUBACK from this client'>In-Flight</th>"
                "<th style='width:13%%' title='PUBLISH packets accepted from this client since it connected'>Published</th>"
                "<th style='width:12%%'>Keep-Alive</th>"
                "</tr>");

            for (int i = 0; i < mqtt_count && pos < PAGE_BUF_SIZE - 256; i++) {
                int conn_sec = (int)(clients[i].connected_ms / 1000);
                int conn_h = conn_sec / 3600;
                int conn_m = (conn_sec % 3600) / 60;
                int conn_s = conn_sec % 60;

                int last_sec = (int)(clients[i].last_active_ms / 1000);

                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<tr><td>%s</td>"
                    "<td><a href='http://%s/' target='_blank' rel='noopener'>%s</a></td>"
                    "<td>%dh %dm %ds</td><td>%ds ago</td>"
                    "<td>%d</td><td>%d</td><td>%lu</td><td>%us</td></tr>",
                    clients[i].client_id[0] ? clients[i].client_id : "<em>(empty)</em>",
                    clients[i].ip, clients[i].ip,
                    conn_h, conn_m, conn_s,
                    last_sec,
                    clients[i].subscriptions,
                    clients[i].inflight,
                    (unsigned long)clients[i].published,
                    clients[i].keep_alive);
            }
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</table>");
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</fieldset>");

        if (clients) free(clients);

        /* WiFi AP clients (devices connected to our SoftAP) */
        wifi_sta_list_t sta_list;
        esp_err_t ap_err = esp_wifi_ap_get_sta_list(&sta_list);

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;WiFi AP Clients (%d)&nbsp;</legend>",
            (ap_err == ESP_OK) ? sta_list.num : 0);

        if (ap_err != ESP_OK || sta_list.num == 0) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<p style='color:#888;text-align:center;padding:10px'>No devices connected to AP</p>");
        } else {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<table><tr>"
                "<th style='width:40%%'>MAC Address</th>"
                "<th style='width:30%%'>RSSI</th>"
                "</tr>");

            for (int i = 0; i < sta_list.num && pos < PAGE_BUF_SIZE - 128; i++) {
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<tr><td>%02X:%02X:%02X:%02X:%02X:%02X</td>"
                    "<td>%d dBm</td></tr>",
                    sta_list.sta[i].mac[0], sta_list.sta[i].mac[1],
                    sta_list.sta[i].mac[2], sta_list.sta[i].mac[3],
                    sta_list.sta[i].mac[4], sta_list.sta[i].mac[5],
                    sta_list.sta[i].rssi);
            }
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</table>");
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</fieldset>");

        /* In-place refresh via /api/clients polling.
         *
         * Previously this page did setTimeout(location.reload, 5000), which:
         *   - destroyed text selection and scroll position every 5s,
         *   - re-rendered ~4 KB of HTML on the ESP32 each cycle,
         *   - made the page unusable while reading a long client list.
         *
         * Now we fetch the JSON, patch the table bodies in place, pause when
         * the tab is hidden (visibilitychange), and show last-updated and live
         * status text the user can actually trust. A <noscript> fallback still
         * does a hard reload for JS-disabled clients. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<p style='color:#888;font-size:0.85em;text-align:center'>"
            "Live <span id='lupd'>--</span> "
            "<button type='button' id='liveBtn' "
            "style='background:#444;color:#eaeaea;border:1px solid #666;"
            "border-radius:3px;padding:2px 8px;font-size:.85em;cursor:pointer;width:auto'>"
            "pause</button></p>"
            "<noscript><meta http-equiv='refresh' content='5'></noscript>"
            "<script>(function(){"
            "var live=true,paused=false,timer=null,IVL=3000;"
            "var lupd=document.getElementById('lupd');"
            "var btn=document.getElementById('liveBtn');"
            "function fmtHMS(s){var h=Math.floor(s/3600),m=Math.floor((s%%3600)/60),x=s%%60;"
            "return h+'h '+m+'m '+x+'s';}"
            "function ts(){var d=new Date();var p=function(n){return(n<10?'0':'')+n;};"
            "return p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());}"
            "function paint(d){"
            "var mt=document.querySelector('fieldset:first-of-type legend');"
            "if(mt){mt.innerHTML='\\u00a0MQTT Clients ('+d.mqtt.length+')\\u00a0';}"
            "var rows=[];d.mqtt.sort(function(a,b){"
            "var aa=parseInt((a.ip||'').split('.').pop(),10)||0;"
            "var bb=parseInt((b.ip||'').split('.').pop(),10)||0;return aa-bb;});"
            "for(var i=0;i<d.mqtt.length;i++){var c=d.mqtt[i];"
            "var tr=document.createElement('tr');"
            "var cid=c.client_id||'(empty)';"
            "tr.innerHTML='<td></td><td><a target=\"_blank\" rel=\"noopener\"></a></td>'+"
            "'<td></td><td></td><td></td><td></td><td></td><td></td>';"
            "var td=tr.children;"
            "td[0].textContent=cid;"
            "td[1].firstChild.textContent=c.ip;td[1].firstChild.href='http://'+c.ip+'/';"
            "td[2].textContent=fmtHMS(c.connected_s);"
            "td[3].textContent=c.last_active_s+'s ago';"
            "td[4].textContent=c.subs;"
            "td[5].textContent=c.inflight;"
            "td[6].textContent=c.published;"
            "td[7].textContent=c.keep_alive+'s';"
            "rows.push(tr);}"
            "var tbl=document.querySelector('fieldset:first-of-type table');"
            "if(tbl){var hdr=tbl.rows[0];tbl.innerHTML='';tbl.appendChild(hdr);"
            "for(var j=0;j<rows.length;j++)tbl.appendChild(rows[j]);}"
            "lupd.textContent='\\u00b7 last update '+ts();}"
            "function poll(){if(!live||paused)return;"
            "fetch('/api/clients',{cache:'no-store'}).then(function(r){return r.json();})"
            ".then(paint).catch(function(){lupd.textContent='\\u00b7 error \\u2014 retrying';});}"
            "function tick(){poll();timer=setTimeout(tick,IVL);}"
            "document.addEventListener('visibilitychange',function(){"
            "if(document.hidden){live=false;}else{live=true;poll();}});"
            "btn.onclick=function(){paused=!paused;btn.textContent=paused?'resume':'pause';if(!paused)poll();};"
            "tick();})();</script>"
            "<br><a href='/' class='btn'>Main Menu</a>");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ CLIENTS JSON API ============ */
    } else if (strcmp(req.path, "/api/clients") == 0) {
        char *json = body;
        int pos = 0;

        /* MQTT clients */
        broker_client_info_t *clients = (broker_client_info_t *)malloc(
            BROKER_MAX_CLIENTS * sizeof(broker_client_info_t));
        int mqtt_count = 0;
        if (clients) {
            mqtt_count = broker_get_clients(clients, BROKER_MAX_CLIENTS);
        }

        pos += snprintf(json + pos, PAGE_BUF_SIZE - pos, "{\"mqtt\":[");
        for (int i = 0; i < mqtt_count && pos < PAGE_BUF_SIZE - 256; i++) {
            if (i > 0) pos += snprintf(json + pos, PAGE_BUF_SIZE - pos, ",");
            pos += snprintf(json + pos, PAGE_BUF_SIZE - pos,
                "{\"client_id\":\"%s\",\"ip\":\"%s\","
                "\"connected_s\":%lld,\"last_active_s\":%lld,"
                "\"subs\":%d,\"inflight\":%d,\"published\":%lu,\"keep_alive\":%u}",
                clients[i].client_id, clients[i].ip,
                (long long)(clients[i].connected_ms / 1000),
                (long long)(clients[i].last_active_ms / 1000),
                clients[i].subscriptions,
                clients[i].inflight,
                (unsigned long)clients[i].published,
                clients[i].keep_alive);
        }
        pos += snprintf(json + pos, PAGE_BUF_SIZE - pos, "],");

        if (clients) free(clients);

        /* WiFi AP clients */
        wifi_sta_list_t sta_list;
        esp_err_t ap_err = esp_wifi_ap_get_sta_list(&sta_list);

        pos += snprintf(json + pos, PAGE_BUF_SIZE - pos, "\"wifi_ap\":[");
        if (ap_err == ESP_OK) {
            for (int i = 0; i < sta_list.num && pos < PAGE_BUF_SIZE - 128; i++) {
                if (i > 0) pos += snprintf(json + pos, PAGE_BUF_SIZE - pos, ",");
                pos += snprintf(json + pos, PAGE_BUF_SIZE - pos,
                    "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
                    sta_list.sta[i].mac[0], sta_list.sta[i].mac[1],
                    sta_list.sta[i].mac[2], sta_list.sta[i].mac[3],
                    sta_list.sta[i].mac[4], sta_list.sta[i].mac[5],
                    sta_list.sta[i].rssi);
            }
        }
        pos += snprintf(json + pos, PAGE_BUF_SIZE - pos, "]}");

        http_response_start(client_fd, "200 OK", "application/json", pos);
        http_send_body(client_fd, json, pos);

    /* ============ FIRMWARE UPDATE PAGE ============ */
    } else if (strcmp(req.path, "/update") == 0) {
        int pos = 0;

        /* Current firmware info */
        const esp_app_desc_t *app = esp_app_get_description();
        const esp_partition_t *running = esp_ota_get_running_partition();

        /* Inspect the inactive OTA slot so we can offer a manual rollback
         * button when it holds a valid app. We do NOT enable bootloader
         * automatic rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) here --
         * adding that without also wiring an in-app self-test would brick
         * users on first upgrade. Manual rollback is the safe middle ground. */
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t other_desc;
        bool other_valid =
            (other && esp_ota_get_partition_description(other, &other_desc) == ESP_OK);

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Firmware Information&nbsp;</legend><table>"
            "<tr><th>Current Version</th><td>" FW_VERSION "</td></tr>"
            "<tr><th>Build Date</th><td>" __DATE__ " " __TIME__ "</td></tr>"
            "<tr><th>IDF Version</th><td>%s</td></tr>"
            "<tr><th>Running Partition</th><td>%s</td></tr>"
            "<tr><th>Other Partition</th><td>%s%s%s%s</td></tr>"
            "</table></fieldset>",
            app->idf_ver,
            running ? running->label : "unknown",
            other ? other->label : "<em>none</em>",
            other_valid ? " \xe2\x80\x94 " : "",
            other_valid ? other_desc.version : (other ? " (empty or invalid)" : ""),
            other_valid && other_desc.project_name[0] ? "" : "");

        /* Rollback panel -- only when the inactive slot has a valid app. */
        if (other_valid) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<fieldset><legend>&nbsp;Rollback&nbsp;</legend>"
                "<p>Switch the boot partition to <b>%s</b> (%s %s) and reboot. "
                "Use this if the current firmware misbehaves. The current image "
                "stays in flash so you can switch back the same way.</p>"
                "<form method='POST' action='/ota-rollback' "
                "onsubmit=\"return confirm('Roll back to ' + '%s' + ' and reboot?')\">"
                "<input type='hidden' name='csrf' value='%s'>"
                "<button type='submit' class='btn bgry'>Roll back &amp; Reboot</button>"
                "</form></fieldset>",
                other->label,
                other_desc.project_name[0] ? other_desc.project_name : FW_NAME,
                other_desc.version,
                other_desc.version,
                csrf_token_hex());
        }

        /* File upload form */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            /* The CSRF token rides in the form `action` URL as a query
             * parameter. Multipart bodies put hidden inputs AFTER files
             * in many browser implementations, which would force us to
             * buffer the entire upload before validating -- much simpler
             * to authenticate via the URL. Same token, same security
             * model. */
            "<fieldset><legend>&nbsp;Upload Firmware&nbsp;</legend>"
            "<form method='POST' action='/ota-upload?csrf=%s' enctype='multipart/form-data' "
            "id='fwform'>"
            "<label>Select firmware .bin file</label>"
            "<input type='file' name='firmware' accept='.bin' required "
            "style='padding:10px;margin:8px 0'>"
            "<br><button type='submit' class='bgrn' id='fwbtn'>Upload &amp; Flash</button>"
            "</form>"
            "<div id='fwprog' style='display:none'>"
            "<div class='st warn'>Uploading firmware... Do not power off!</div>"
            "<progress id='pbar' value='0' max='100' style='width:100%%;height:20px;margin:8px 0'>"
            "</progress><span id='pct'>0%%</span>"
            "</div>"
            "<script>"
            "document.getElementById('fwform').onsubmit=function(e){"
            "e.preventDefault();"
            "var f=document.querySelector('input[type=file]').files[0];"
            "if(!f){alert('Select a file');return;}"
            "document.getElementById('fwprog').style.display='block';"
            "document.getElementById('fwbtn').disabled=true;"
            "var x=new XMLHttpRequest();"
            "x.upload.onprogress=function(e){"
            "if(e.lengthComputable){"
            "var p=Math.round(e.loaded/e.total*100);"
            "document.getElementById('pbar').value=p;"
            "document.getElementById('pct').textContent=p+'%%';"
            "}};"
            "x.onload=function(){"
            "if(x.status==200){"
            "document.getElementById('fwprog').innerHTML="
            "\"<div class='st ok'>Upload complete! Switching to reboot view...</div>\";"
            /* Redirect to the standalone countdown page immediately. The
             * device started restarting the moment it sent us 200; the
             * countdown page polls /api/status and swaps itself for a
             * 'Back online' link as soon as the new firmware answers. */
            "location.href='/rebooting';"
            "}else{"
            "document.getElementById('fwprog').innerHTML="
            "\"<div class='st warn'>Upload failed: \"+x.responseText+\"</div>\";"
            "document.getElementById('fwbtn').disabled=false;}};"
            "x.onerror=function(){"
            "document.getElementById('fwprog').innerHTML="
            "\"<div class='st warn'>Connection error</div>\";"
            "document.getElementById('fwbtn').disabled=false;};"
            "var fd=new FormData();fd.append('firmware',f);"
            "x.open('POST','/ota-upload?csrf=%s');"
            "x.send(fd);"
            "};"
            "</script>"
            "</fieldset>",
            csrf_token_hex(),    /* %s in form action */
            csrf_token_hex());   /* %s in x.open() URL  */

        /* URL-based OTA.
         *
         * Both http:// and https:// are accepted. https:// uses the ESP-IDF
         * default TLS stack with system CA bundle (when configured); on a
         * trusted LAN plain http is usually fine. The HTML5 pattern was
         * previously `http://.*` which silently blocked https — confusing
         * users with self-hosted release servers behind TLS. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;OTA Update via URL&nbsp;</legend>"
            "<form method='POST' action='/ota-url' id='urlform'>"
            "<input type='hidden' name='csrf' value='%s'>"
            "<label>Firmware URL (http:// or https://)</label>"
            "<input type='url' name='url' placeholder='http://192.168.1.100:8080/firmware.bin' "
            "required pattern='https?://.*'>"
            "<br><button type='submit' class='bgrn'>Download &amp; Flash</button>"
            "</form></fieldset>",
            csrf_token_hex());

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<br><a href='/' class='btn'>Main Menu</a>");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ LIVENESS PING ============
     * Minimal endpoint used by the reboot-countdown polling loop. Always
     * exempt from Basic Auth (see the auth-check block above). Returns
     * only uptime so it leaks nothing about settings, network, or topology.
     * The countdown JS uses this for two signals: (a) any HTTP response =
     * device is alive, (b) uptime regression = fresh boot detected. */
    } else if (strcmp(req.path, "/api/ping") == 0) {
        broker_stats_t stats;
        broker_get_stats(&stats);
        char *json = body;
        int len = snprintf(json, PAGE_BUF_SIZE,
            "{\"uptime_s\":%lld}",
            (long long)(stats.uptime_ms / 1000));
        http_response_start(client_fd, "200 OK", "application/json", len);
        http_send_body(client_fd, json, len);

    /* ============ CSRF TOKEN (auth-gated) ============
     * CLI helper. Browsers see the same token on every page in their
     * `csrf` cookie and don't need this endpoint -- but `curl` and
     * `make ota` do, because cookies aren't preserved between separate
     * invocations without --cookie-jar.
     *
     * Returns the active token as JSON; the response also re-emits the
     * `Set-Cookie: csrf=...` header via http_response_start, so a
     * cookie-jar-using client gets it twice. Auth-gated (Basic Auth
     * already enforced upstream) so an attacker can't lift the token
     * with one request. */
    } else if (strcmp(req.path, "/api/csrf") == 0 && req.method == REQ_GET) {
        char *json = body;
        int len = snprintf(json, PAGE_BUF_SIZE,
            "{\"token\":\"%s\"}", csrf_token_hex());
        http_response_start(client_fd, "200 OK", "application/json", len);
        http_send_body(client_fd, json, len);

    /* ============ NTP STATE (open) ============
     * Phase 1 of plan-ntp-server.md. Returns the bare facts about the
     * SNTP client state so clients can verify wall-clock freshness
     * without having to add a Basic Auth dependency to a check. The
     * upstream name is included for transparency; if you don't want it
     * exposed, set ntp.enabled=0 in NVS (the field becomes empty). */
    } else if (strcmp(req.path, "/api/time") == 0 && req.method == REQ_GET) {
        ntp_state_t st;
        ntp_get_state(&st);
        /* Escape the hostname for safe JSON embedding (quotes/backslashes
         * only -- our hostnames are otherwise ASCII). */
        char host_esc[NTP_UPSTREAM_MAX_LEN * 2];
        size_t oi = 0;
        for (size_t ii = 0; st.upstream_used[ii] && oi < sizeof(host_esc) - 2; ii++) {
            char c = st.upstream_used[ii];
            if (c == '"' || c == '\\') host_esc[oi++] = '\\';
            host_esc[oi++] = c;
        }
        host_esc[oi] = '\0';
        char *json = body;
        /* Server-side fields (stratum, served, dropped_*) are zero in
         * pre-Phase-2 builds; populated once ntp_server_start() succeeds.
         * Splitting client and server sub-objects keeps the JSON
         * self-documenting and lets dashboards distinguish 'we have time'
         * from 'we provide time'. */
        /* drift_ppm: null when we don't yet have a valid estimate
         * (< 2 syncs or baseline < 60s). free_running_s: 0 when within
         * the normal poll interval. Both new in 0.7.2. */
        char drift_json[32];
        if (st.drift_ppm == INT32_MIN) {
            snprintf(drift_json, sizeof(drift_json), "null");
        } else {
            snprintf(drift_json, sizeof(drift_json), "%ld", (long)st.drift_ppm);
        }
        int len = snprintf(json, PAGE_BUF_SIZE,
            "{\"synced\":%s,\"epoch_us\":%lld,\"last_sync_age_s\":%lld,"
            "\"sync_count\":%u,\"upstream\":\"%s\","
            "\"server_running\":%s,\"stratum\":%u,\"served\":%u,"
            "\"dropped\":{\"rate\":%u,\"size\":%u,\"mode\":%u},"
            "\"drift_ppm\":%s,\"free_running_s\":%ld}",
            st.synced ? "true" : "false",
            (long long)st.epoch_us,
            (long long)st.last_sync_age_s,
            (unsigned)st.sync_count,
            host_esc,
            st.server_running ? "true" : "false",
            (unsigned)st.stratum,
            (unsigned)st.served,
            (unsigned)st.dropped_rate,
            (unsigned)st.dropped_size,
            (unsigned)st.dropped_mode,
            drift_json,
            (long)st.free_running_s);
        http_response_start(client_fd, "200 OK", "application/json", len);
        http_send_body(client_fd, json, len);

    /* ============ NTP RESYNC (gated) ============
     * Forces an immediate upstream poll. Returns the post-resync state
     * after a short delay (the SNTP callback fires on the esp_sntp task
     * a few hundred ms later, so we report 'in-flight' if it hasn't
     * landed yet). Per the plan this is auth-gated; the regular Basic
     * Auth check above already enforces that. */
    /* ============ NTP MANUAL SET (gated, opt-in) ============
     * Phase 4 of plan-ntp-server.md. Body is either JSON ({"epoch_us":N}
     * or {"epoch_s":N}) or form-encoded (epoch_us=N / epoch_s=N) so a
     * curl one-liner or the /time form both work. Refused unless the
     * operator has flipped `ntp.accept_set` on in /settings AND the
     * device is currently unsynced -- ntp_manual_set() enforces both. */
    } else if (strcmp(req.path, "/api/time/set") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        /* Extract epoch_us (preferred) or epoch_s (form convenience).
         * We don't pull in a JSON parser for this; the body is small and
         * the field name is unambiguous -- find it, skip ':' or '=',
         * skip whitespace/quotes, strtoll. */
        int64_t epoch_us = 0;
        const char *b = req.body;
        const char *p = strstr(b, "epoch_us");
        if (p) {
            p += strlen("epoch_us");
            while (*p == ' ' || *p == ':' || *p == '=' || *p == '"' || *p == '\t') p++;
            epoch_us = (int64_t)strtoll(p, NULL, 10);
        } else if ((p = strstr(b, "epoch_s")) != NULL) {
            p += strlen("epoch_s");
            while (*p == ' ' || *p == ':' || *p == '=' || *p == '"' || *p == '\t') p++;
            epoch_us = (int64_t)strtoll(p, NULL, 10) * 1000000LL;
        }

        ntp_manual_result_t r = ntp_manual_set(epoch_us);
        const char *status_line;
        const char *err_str;
        switch (r) {
            case NTP_MANUAL_OK:
                status_line = "200 OK"; err_str = "ok"; break;
            case NTP_MANUAL_DISABLED:
                status_line = "403 Forbidden"; err_str = "disabled"; break;
            case NTP_MANUAL_ALREADY_SYNCED:
                status_line = "409 Conflict"; err_str = "already_synced"; break;
            case NTP_MANUAL_BAD_EPOCH:
            default:
                status_line = "400 Bad Request"; err_str = "bad_epoch"; break;
        }
        /* Echo the accepted epoch so callers can confirm what landed --
         * useful when the JSON parse went sideways and silently grabbed
         * the wrong number. */
        char *json = body;
        int len = snprintf(json, PAGE_BUF_SIZE,
            "{\"ok\":%s,\"status\":\"%s\",\"epoch_us\":%lld}",
            (r == NTP_MANUAL_OK) ? "true" : "false",
            err_str, (long long)epoch_us);
        if (r == NTP_MANUAL_OK) {
            ESP_LOGW(TAG, "Manual time set accepted: epoch_us=%lld",
                     (long long)epoch_us);
        } else {
            ESP_LOGW(TAG, "Manual time set rejected (%s): epoch_us=%lld",
                     err_str, (long long)epoch_us);
        }
        http_response_start(client_fd, status_line, "application/json", len);
        http_send_body(client_fd, json, len);

    } else if (strcmp(req.path, "/api/time/resync") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        bool ok = ntp_force_resync();
        char *json = body;
        int len = snprintf(json, PAGE_BUF_SIZE,
            "{\"triggered\":%s}", ok ? "true" : "false");
        http_response_start(client_fd, ok ? "200 OK" : "503 Service Unavailable",
                            "application/json", len);
        http_send_body(client_fd, json, len);

    /* ============ /time PAGE (Phase 3 of plan-ntp-server.md) ============
     * Tasmota-style server-rendered HTML. No JS required; uses a
     * <meta http-equiv='refresh' content='10'> so the clock and counters
     * stay fresh without polling JS. Shows:
     *   - Big current-time line (UTC, ISO 8601)
     *   - Client/server status badges (stratum, sync age, server state)
     *   - Force-resync button (POST /api/time/resync, GET-safe redirect)
     *   - Recent-clients table from the rate-limit LRU (last 16 by activity)
     * Auth-gated like every other portal page; the /api/time JSON
     * endpoint stays open for programmatic clients. */
    } else if (strcmp(req.path, "/time") == 0 && req.method == REQ_GET) {
        ntp_state_t st;
        ntp_get_state(&st);

        int pos = 0;
        /* Auto-refresh every 10s so the clock + counters stay current. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<meta http-equiv='refresh' content='10'>");

        /* Big current-time block. ISO 8601 in UTC; compact (year, hh:mm:ss). */
        char iso[40] = "-- not yet synced --";
        if (st.synced && st.epoch_us > 0) {
            time_t t = (time_t)(st.epoch_us / 1000000);
            struct tm tm;
            gmtime_r(&t, &tm);
            strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S UTC", &tm);
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Now&nbsp;</legend>"
            "<p style='font-family:monospace;font-size:1.4em;margin:8px 0;text-align:center;"
            "color:%s'>%s</p>"
            "<p style='text-align:center;font-size:0.85em;color:#aaa;margin:0'>"
            "epoch %lld\xc2\xb5s</p>"
            "</fieldset>",
            st.synced ? "#a5d6a7" : "#ffcc80",
            iso, (long long)st.epoch_us);

        /* Client + server status cards. Sized to accommodate the longest
         * synced variant: ~80 chars of fixed text + up to 63 chars of
         * upstream hostname + a couple of numbers. 256 is generous and
         * silences -Wformat-truncation. */
        char client_line[256], server_line[512];
        if (st.synced) {
            snprintf(client_line, sizeof(client_line),
                "<span style='color:#a5d6a7'>synced</span> \xc2\xb7 "
                "last %llds ago \xc2\xb7 %u total \xc2\xb7 upstream <b>%s</b>",
                (long long)st.last_sync_age_s, (unsigned)st.sync_count,
                st.upstream_used[0] ? st.upstream_used : "-");
        } else {
            snprintf(client_line, sizeof(client_line),
                "<span style='color:#ffcc80'>not yet synced</span> \xc2\xb7 "
                "upstream <b>%s</b>",
                st.upstream_used[0] ? st.upstream_used : "-");
        }
        if (st.server_running) {
            /* 0.7.2: surface drift + free-running state. drift line is
             * suffixed only when we have a measurement; when unknown
             * we just omit it rather than show 'drift: ?'. */
            char drift_suffix[160] = "";
            if (st.drift_ppm != INT32_MIN) {
                if (st.free_running_s > 0) {
                    snprintf(drift_suffix, sizeof(drift_suffix),
                        " \xc2\xb7 drift %+ld ppm \xc2\xb7 "
                        "<span style='color:#ffcc80'>free-running %ds</span>",
                        (long)st.drift_ppm, (int)st.free_running_s);
                } else {
                    snprintf(drift_suffix, sizeof(drift_suffix),
                        " \xc2\xb7 drift %+ld ppm",
                        (long)st.drift_ppm);
                }
            }
            snprintf(server_line, sizeof(server_line),
                "<span style='color:#a5d6a7'>serving</span> on UDP:123 \xc2\xb7 "
                "stratum %u \xc2\xb7 %u served \xc2\xb7 "
                "dropped %u/%u/%u (rate/size/mode)%s",
                (unsigned)st.stratum, (unsigned)st.served,
                (unsigned)st.dropped_rate, (unsigned)st.dropped_size,
                (unsigned)st.dropped_mode,
                drift_suffix);
        } else {
            snprintf(server_line, sizeof(server_line),
                "<span style='color:#888'>server off</span>");
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Client&nbsp;</legend><p>%s</p></fieldset>"
            "<fieldset><legend>&nbsp;Server&nbsp;</legend><p>%s</p></fieldset>",
            client_line, server_line);

        /* Force-resync button. Wraps a POST in a form so we don't need JS;
         * after the POST the browser follows the JSON response, so we add
         * a tiny JS handler purely to do a redirect back to /time. The
         * <noscript> fallback (a plain action='/api/time/resync' form)
         * leaves the user on the JSON output but still triggers the action. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<form id='rsf' method='POST' action='/api/time/resync' style='margin:8px 0'>"
            "<input type='hidden' name='csrf' value='%s'>"
            "<button type='submit' class='btn'>Force resync</button>"
            "</form>"
            /* fetch() path uses X-CSRF-Token header; <noscript> users
             * get the <form> path with the hidden field above. Same
             * token, two carriers. */
            "<script>document.getElementById('rsf').onsubmit=function(e){"
            "e.preventDefault();"
            "fetch('/api/time/resync',"
            "{method:'POST',headers:{'X-CSRF-Token':'%s'}})"
            ".then(function(){setTimeout(function(){location.href='/time';},800);});"
            "return false;};</script>",
            csrf_token_hex(), csrf_token_hex());

        /* ====== Manual time-set form (Phase 4 of plan-ntp-server.md) ======
         * Only renders when (a) the operator has opted in via
         * /settings -> Accept manual time set, AND (b) we are currently
         * unsynced -- the same gates ntp_manual_set() enforces server
         * side. Rendering the form when the server would refuse it would
         * just be confusing. The browser-time button is the common path
         * (one click, microsecond precision from Date.now()); the
         * datetime-local input is a fallback for when the browsing
         * device's own clock is wrong (e.g. tethered to the broker AP
         * with no other reference). Both POST to /api/time/set with
         * X-CSRF-Token; no JSON parser is required server-side. */
        {
            uint8_t accept_set = 0;
            nvs_handle_t snh;
            if (nvs_open("ntp", NVS_READONLY, &snh) == ESP_OK) {
                nvs_get_u8(snh, "accept_set", &accept_set);
                nvs_close(snh);
            }
            if (accept_set && !st.synced) {
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<fieldset><legend>&nbsp;Set time manually&nbsp;</legend>"
                    "<p style='color:#ffcc80;font-size:0.85em;margin:0 0 8px 0'>"
                    "No upstream reachable. Set the clock from this browser "
                    "so retained-message timestamps and $SYS/broker/time "
                    "start working. A real upstream sync will supersede this "
                    "value automatically.</p>"
                    "<button id='tsnow' type='button' class='btn' "
                    "style='margin-bottom:8px'>Use this browser's clock</button>"
                    "<p style='color:#aaa;font-size:0.8em;margin:4px 0'>"
                    "\xe2\x80\x94 or pick a UTC moment \xe2\x80\x94</p>"
                    "<input type='datetime-local' id='tsdt' step='1' "
                    "style='width:100%%;margin-bottom:6px'>"
                    "<button id='tsmanual' type='button' class='btn'>"
                    "Set to entered time (UTC)</button>"
                    "<p id='tsmsg' style='margin-top:8px;min-height:1.2em'></p>"
                    "<script>"
                    "function tsPost(us){"
                    "var m=document.getElementById('tsmsg');"
                    "m.textContent='Submitting\xe2\x80\xa6';m.style.color='#aaa';"
                    "fetch('/api/time/set',{method:'POST',"
                    "headers:{'X-CSRF-Token':'%s','Content-Type':'application/json'},"
                    "body:JSON.stringify({epoch_us:us})})"
                    ".then(function(r){return r.json().then(function(j){return{r:r,j:j};});})"
                    ".then(function(o){if(o.r.ok){m.textContent='Accepted. Reloading\xe2\x80\xa6';"
                    "m.style.color='#a5d6a7';setTimeout(function(){location.href='/time';},900);}"
                    "else{m.textContent='Rejected: '+(o.j.status||o.r.status);"
                    "m.style.color='#ff8888';}})"
                    ".catch(function(e){m.textContent='Network error: '+e;m.style.color='#ff8888';});}"
                    "document.getElementById('tsnow').onclick=function(){"
                    "tsPost(Date.now()*1000);};"
                    "document.getElementById('tsmanual').onclick=function(){"
                    "var v=document.getElementById('tsdt').value;"
                    "if(!v){document.getElementById('tsmsg').textContent='Enter a date/time first.';"
                    "document.getElementById('tsmsg').style.color='#ff8888';return;}"
                    "var ms=Date.parse(v+'Z');"
                    "if(isNaN(ms)){document.getElementById('tsmsg').textContent='Could not parse.';"
                    "document.getElementById('tsmsg').style.color='#ff8888';return;}"
                    "tsPost(ms*1000);};"
                    "</script>"
                    /* <noscript> path: vanilla form posting epoch_s
                     * from a hidden field the operator fills out
                     * (rare; documented for completeness). */
                    "<noscript><p style='color:#aaa;font-size:0.85em'>"
                    "Without JavaScript: <code>curl -u user:pass "
                    "-H 'X-CSRF-Token: %s' -d 'epoch_s=$(date +%%s)' "
                    "http://&lt;device&gt;/api/time/set</code></p></noscript>"
                    "</fieldset>",
                    csrf_token_hex(), csrf_token_hex());
            } else if (!st.synced) {
                /* Hint discoverability when the operator has NOT opted in.
                 * Tiny line, no form -- this is informational only. */
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<p style='color:#888;font-size:0.8em;text-align:center'>"
                    "No upstream reachable. To allow manual time set from "
                    "this page, enable it in <a href='/settings'>Settings "
                    "\xe2\x86\x92 Time (NTP)</a>.</p>");
            }
        }

        /* Recent-clients table from the server's rate-limit LRU. Sorted
         * by last_us descending in-place (simple insertion sort -- max 32
         * entries, O(n^2) is fine and avoids a qsort link dependency). */
        ntp_recent_client_t recent[NTP_RECENT_MAX];
        int rn = ntp_get_recent_clients(recent, NTP_RECENT_MAX);
        for (int i = 1; i < rn; i++) {
            ntp_recent_client_t k = recent[i];
            int j = i - 1;
            while (j >= 0 && recent[j].last_us < k.last_us) {
                recent[j + 1] = recent[j];
                j--;
            }
            recent[j + 1] = k;
        }

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Recent clients (%d)&nbsp;</legend>", rn);
        if (rn == 0) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<p style='color:#888;text-align:center;padding:10px'>"
                "No SNTP queries received yet</p>");
        } else {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<table><tr>"
                "<th style='width:55%%'>Source IP</th>"
                "<th style='width:20%%'>Last</th>"
                "<th style='width:25%%'>Total</th></tr>");
            int64_t now_us = esp_timer_get_time();
            /* Cap rendered rows at 16 to keep the page tidy. */
            int show = rn < 16 ? rn : 16;
            for (int i = 0; i < show && pos < (int)PAGE_BUF_SIZE - 256; i++) {
                uint32_t a = recent[i].addr;
                /* sin_addr.s_addr is network byte order: byte 0 is the
                 * first octet. */
                uint8_t o0 = a & 0xff, o1 = (a >> 8) & 0xff,
                        o2 = (a >> 16) & 0xff, o3 = (a >> 24) & 0xff;
                int64_t age_s = (now_us - recent[i].last_us) / 1000000;
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<tr><td>%u.%u.%u.%u</td><td>%llds ago</td><td>%u</td></tr>",
                    o0, o1, o2, o3, (long long)age_s, (unsigned)recent[i].total);
            }
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</table>");
            if (rn > show) {
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<p style='color:#888;font-size:0.8em;text-align:center'>"
                    "(%d more not shown)</p>", rn - show);
            }
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</fieldset>");

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<p style='color:#888;font-size:0.85em;text-align:center'>"
            "Page auto-refreshes every 10 seconds</p>"
            "<br><a href='/' class='btn'>Main Menu</a>");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ JSON API ============ */
    } else if (strcmp(req.path, "/api/status") == 0) {
        broker_stats_t stats;
        broker_get_stats(&stats);

        int sta_connected = 0, ap_running = 0;
        char ip_str[16] = "", ssid_str[33] = "";
        portal_get_sta_status(&sta_connected, &ap_running, ip_str, sizeof(ip_str),
                              ssid_str, sizeof(ssid_str));

        char *json = body;  /* reuse heap buffer */
        int len = snprintf(json, PAGE_BUF_SIZE,
            "{\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":%s},"
            "\"broker\":{\"clients\":%d,\"max_clients\":%d,\"subs\":%d,"
            "\"retained\":%lu,\"retained_kb\":%lu,\"port\":%d},"
            "\"firmware\":{\"name\":\"" FW_NAME "\",\"version\":\"" FW_VERSION "\","
            "\"build\":\"" __DATE__ " " __TIME__ "\"},"
            "\"system\":{\"uptime_s\":%lld,\"free_heap_kb\":%lu}",
            sta_connected ? "true" : "false", ssid_str, ip_str,
            ap_running ? "true" : "false",
            stats.connected_clients, BROKER_MAX_CLIENTS,
            stats.active_subscriptions,
            (unsigned long)stats.retained_count, (unsigned long)(stats.retained_bytes / 1024),
            BROKER_PORT,
            (long long)(stats.uptime_ms / 1000),
            (unsigned long)(stats.free_heap / 1024));

#ifdef CONFIG_MQTT_BROKER_ETHERNET
        {
            char eth_ip[16] = "";
            int eth_up = eth_is_connected();
            if (eth_up) eth_get_ip_str(eth_ip, sizeof(eth_ip));
            /* Escape last-error string minimally (quotes → ') */
            char err_esc[96];
            const char *src = eth_get_last_error();
            size_t ei = 0;
            for (size_t si = 0; src[si] && ei < sizeof(err_esc) - 1; si++) {
                char c = src[si];
                err_esc[ei++] = (c == '"' || c == '\\') ? '\'' : c;
            }
            err_esc[ei] = '\0';
            len += snprintf(json + len, PAGE_BUF_SIZE - len,
                ",\"ethernet\":{\"connected\":%s,\"ip\":\"%s\",\"napt\":%s,"
                "\"link\":%s,\"stage\":\"%s\",\"last_error\":\"%s\","
                "\"pins\":{\"mosi\":%d,\"miso\":%d,\"sclk\":%d,"
                "\"cs\":%d,\"int\":%d,\"rst\":%d,\"clock_mhz\":%d}}",
                eth_up ? "true" : "false", eth_ip,
                eth_napt_is_enabled() ? "true" : "false",
                eth_is_link_up() ? "true" : "false",
                eth_get_stage(), err_esc,
                CONFIG_ETH_SPI_MOSI, CONFIG_ETH_SPI_MISO, CONFIG_ETH_SPI_SCLK,
                CONFIG_ETH_SPI_CS, CONFIG_ETH_SPI_INT, CONFIG_ETH_SPI_RST,
                CONFIG_ETH_SPI_CLOCK_MHZ);
        }
#endif
        len += snprintf(json + len, PAGE_BUF_SIZE - len, "}");

        http_response_start(client_fd, "200 OK", "application/json", len);
        http_send_body(client_fd, json, len);

    /* ============ OTA ROLLBACK ============
     * Sets the boot partition to the *other* OTA slot (if it has a valid
     * app) and reboots. Implements manual rollback without requiring
     * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. Idempotent: pressing twice
     * just bounces the user between slots. */
    } else if (strcmp(req.path, "/ota-rollback") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t other_desc;
        if (!other || esp_ota_get_partition_description(other, &other_desc) != ESP_OK) {
            int len = snprintf(body, PAGE_BUF_SIZE,
                "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta http-equiv='refresh' content='3;url=/update'></head>"
                "<body style='font-family:sans-serif;padding:40px;text-align:center;"
                "background:#252525;color:#eaeaea'>"
                "<h2>No rollback target</h2>"
                "<p>The other partition is empty or invalid.</p></body></html>");
            http_response_start(client_fd, "400 Bad Request", "text/html; charset=utf-8", len);
            http_send_body(client_fd, body, len);
        } else {
            esp_err_t rerr = esp_ota_set_boot_partition(other);
            if (rerr != ESP_OK) {
                ESP_LOGE(TAG, "Rollback set_boot failed: %s", esp_err_to_name(rerr));
                int len = snprintf(body, PAGE_BUF_SIZE,
                    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                    "<meta http-equiv='refresh' content='3;url=/update'></head>"
                    "<body style='font-family:sans-serif;padding:40px;text-align:center;"
                    "background:#252525;color:#eaeaea'>"
                    "<h2>Rollback failed</h2><p>%s</p></body></html>",
                    esp_err_to_name(rerr));
                http_response_start(client_fd, "500 Internal Server Error", "text/html; charset=utf-8", len);
                http_send_body(client_fd, body, len);
            } else {
                ESP_LOGW(TAG, "OTA rollback to %s (%s) on user request",
                         other->label, other_desc.version);
                char rb_title[48], rb_subtitle[96];
                snprintf(rb_title, sizeof(rb_title),
                         "Rolling back to %.32s", other_desc.version);
                snprintf(rb_subtitle, sizeof(rb_subtitle),
                         "Switching boot partition to %.16s. Reconnect in a few seconds.",
                         other->label);
                http_send_reboot_countdown(client_fd, rb_title, rb_subtitle, "/");
                free(body);
                close(client_fd);
                vTaskDelay(pdMS_TO_TICKS(800));
                esp_restart();
                return;
            }
        }

    /* ============ OTA VIA URL ============ */
    } else if (strcmp(req.path, "/ota-url") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        char url[256] = "";
        urldecode_param(req.body, "url", url, sizeof(url));

        if (url[0] == '\0' ||
            (strncmp(url, "http://",  7) != 0 &&
             strncmp(url, "https://", 8) != 0)) {
            int len = snprintf(body, PAGE_BUF_SIZE,
                "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta http-equiv='refresh' content='3;url=/update'>"
                "</head><body style='font-family:sans-serif;padding:40px;text-align:center;"
                "background:#252525;color:#eaeaea'>"
                "<h2>Invalid URL</h2><p>Must start with http:// or https://</p>"
                "</body></html>");
            http_response_start(client_fd, "400 Bad Request", "text/html; charset=utf-8", len);
            http_send_body(client_fd, body, len);
        } else {
            /* Send a "downloading" page, then start OTA.
             * Note: handle_ota_url will close the fd and reboot on success. */
            free(body);
            handle_ota_url(client_fd, url);
            return;  /* handle_ota_url closes client_fd */
        }

    /* ============ /timers PAGES + API (scheduled MQTT publishes) ============
     * Tasmota-style timer UI backed by main/timers.c. JSON wire format is
     * documented in plan-scheduled-publishes.md §2/§2a. */
    } else if (strcmp(req.path, "/timers") == 0 && req.method == REQ_GET) {
        int pos = 0;

        time_t now_utc = time(NULL);
        bool clock_ok = now_utc >= 1700000000;
        char now_str[64];
        portal_format_local_time(now_utc, now_str, sizeof(now_str));

        if (!clock_ok) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<div class='st warn'>Clock not synced — timers are paused. "
                "<a href='/time' style='color:#fff;text-decoration:underline'>Check NTP</a></div>");
        }

        bool master = timers_master_enabled();
        int armed = 0;
        for (int i = 1; i <= TIMERS_SLOT_COUNT; i++) {
            timer_slot_t t;
            if (timers_get(i, &t) && t.arm) armed++;
        }
        /* 0.8.2 P1 #6 + #7:
         * - Master pause is now an inline clickable pill in the header line,
         *   not a full-width button. Posts to /timers/master via a tiny
         *   <form> wrapped around the pill text.
         * - Below 600px viewport, the 7-column table becomes a card layout:
         *   each row is a stacked block with column labels rendered via
         *   ::before. No JS — pure media query, fallback degrades to the
         *   regular table on browsers that ignore the media block. The
         *   .tg-* classes are scoped to this page so they don't bleed into
         *   the rest of the portal. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<style>"
            /* Override the portal-wide `button{width:100%;line-height:2.4rem;
             * font-size:1.2rem}` defaults so the master pill stays inline. */
            ".tmaster{display:inline-block !important;width:auto !important;"
            "padding:2px 10px !important;border-radius:12px;font-size:0.85em "
            "!important;line-height:1.6 !important;cursor:pointer;border:0;"
            "font-family:inherit}"
            ".tmaster.on{background:#1b5e20;color:#a5d6a7}"
            ".tmaster.off{background:#7a2222;color:#ffb0b0}"
            ".tmaster.on:hover{background:#256528;color:#fff}"
            ".tmaster.off:hover{background:#a02828;color:#fff}"
            ".tmaster form{display:inline;margin:0}"
            "table.tg th,table.tg td{padding:4px 6px;text-align:left;vertical-align:top}"
            "@media (max-width:600px){"
            "table.tg thead{display:none}"
            "table.tg tr{display:block;border-bottom:1px solid #444;padding:6px 0}"
            "table.tg tr:last-child{border-bottom:0}"
            "table.tg td{display:block;padding:2px 0;border:0;width:auto !important}"
            "table.tg td::before{content:attr(data-label) ': ';color:#888;"
            "font-size:0.8em;margin-right:6px}"
            "table.tg td.tg-hdr::before{content:''}"
            "table.tg td.tg-hdr{font-weight:bold;font-size:1.05em;color:#1fa3ec}"
            "}"
            "</style>"
            "<fieldset><legend>&nbsp;Timers&nbsp;</legend>"
            "<p style='margin:4px 0'>%d of %d armed \xc2\xb7 "
            "<form method='POST' action='/timers/master' style='display:inline;margin:0'>"
            "<input type='hidden' name='csrf' value='%s'>"
            "<input type='hidden' name='enable' value='%d'>"
            "<button type='submit' class='tmaster %s' title='Click to %s all timers'>"
            "master: %s</button>"
            "</form>"
            " \xc2\xb7 local <code>%s</code></p>"
            "<table class='tg'><thead><tr>"
            "<th style='width:5%%'>#</th>"
            "<th style='width:7%%' title='● armed \xc2\xb7 ◐ disarmed \xc2\xb7 — empty'>On</th>"
            "<th style='width:24%%'>Label</th>"
            "<th style='width:10%%'>Time</th>"
            "<th style='width:6%%' title='↻ repeats \xc2\xb7 1× once'>Rep</th>"
            "<th style='width:16%%'>Days</th>"
            "<th style='width:32%%'>Topic</th>"
            "</tr></thead><tbody>",
            armed, TIMERS_SLOT_COUNT,
            csrf_token_hex(),
            master ? 0 : 1,
            master ? "on" : "off",
            master ? "pause" : "resume",
            master ? "enabled" : "PAUSED",
            now_str);

        for (int i = 1; i <= TIMERS_SLOT_COUNT; i++) {
            timer_slot_t t;
            timers_get(i, &t);
            char days_str[8] = "-------";
            timers_days_to_string(t.days, days_str);
            bool empty = !t.arm && t.topic[0] == '\0' && t.label[0] == '\0';
            if (empty) {
                /* P1 #5: — (grey) = empty slot. Card-layout note: at
                 * < 600px the colspan stops mattering; data-label drives
                 * the per-cell label rendering instead. */
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<tr style='color:#888'>"
                    "<td class='tg-hdr' data-label='#'>%d</td>"
                    "<td data-label='On'>—</td>"
                    "<td data-label='Label' colspan='4'>"
                    "<a href='/timers/edit?n=%d'>(empty — configure)</a></td>"
                    "<td data-label='Topic'>—</td></tr>",
                    i, i);
            } else {
                char topic_short[40];
                size_t tn = strnlen(t.topic, TIMERS_TOPIC_MAX);
                if (tn > sizeof(topic_short) - 1) {
                    memcpy(topic_short, t.topic, sizeof(topic_short) - 4);
                    memcpy(topic_short + sizeof(topic_short) - 4, "...", 4);
                } else {
                    memcpy(topic_short, t.topic, tn);
                    topic_short[tn] = '\0';
                }
                /* P1 #5: three states for the On indicator.
                 *   ● green  = armed (will fire)
                 *   ◐ orange = configured but disarmed (won't fire)
                 *   — grey    = empty (handled in the branch above)
                 * P1 #4: "Rep" is its own column — ↻ for repeating, 1× for once. */
                const char *on_html = t.arm
                    ? "<span style='color:#a5d6a7' title='armed'>●</span>"
                    : "<span style='color:#ffcc80' title='configured but disarmed'>◐</span>";
                const char *rep_html = t.repeat
                    ? "<span title='repeats'>↻</span>"
                    : "<span style='color:#aaa' title='fires once then disarms'>1×</span>";
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<tr>"
                    "<td class='tg-hdr' data-label='#'>%d</td>"
                    "<td data-label='On'>%s</td>"
                    "<td data-label='Label'><a href='/timers/edit?n=%d'>%s</a></td>"
                    "<td data-label='Time'>%02u:%02u</td>"
                    "<td data-label='Rep'>%s</td>"
                    "<td data-label='Days'><code>%s</code></td>"
                    "<td data-label='Topic'><code style='font-size:0.85em'>%s</code></td>"
                    "</tr>",
                    i, on_html,
                    i, t.label[0] ? t.label : "(unnamed)",
                    (unsigned)(t.minute_of_day / 60),
                    (unsigned)(t.minute_of_day % 60),
                    rep_html,
                    days_str,
                    topic_short);
            }
            if (pos > PAGE_BUF_SIZE - 1024) break;  /* defensive */
        }
        /* P1 #8: drop "Click a row to edit" filler; show "N dropped fires"
         * only when N > 0 — zero is the boring case and noise. */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "</tbody></table>");
        uint32_t dropped = timers_dropped_count();
        if (dropped > 0) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<p style='color:#ffcc80;font-size:0.8em;text-align:center;margin:8px 0 0'>"
                "%u dropped fire(s) since boot</p>", (unsigned)dropped);
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "</fieldset>"
            "<br><a href='/' class='btn'>Main Menu</a>");

        http_send_page(client_fd, body, (size_t)pos);

    } else if (strcmp(req.path, "/timers/edit") == 0 && req.method == REQ_GET) {
        /* Parse ?n=<slot> from query string. */
        int slot = 0;
        const char *n_eq = strstr(req.query, "n=");
        if (n_eq) slot = atoi(n_eq + 2);
        if (slot < 1 || slot > TIMERS_SLOT_COUNT) {
            http_send_plain(client_fd, "400 Bad Request", "slot out of range");
            free(body); close(client_fd); return;
        }
        timer_slot_t t;
        timers_get(slot, &t);
        char days_str[8] = "SMTWTFS";
        timers_days_to_string(t.days ? t.days : TIMERS_DAYS_ALL, days_str);

        /* P0 #3: empty slot → render time <input> with no value so the
         * browser shows it empty rather than a misleading "12:00 AM"
         * default. "Truly empty" = no label, no topic, no arm, midnight
         * minute_of_day — exactly the cleared state. Once the user fills
         * any field we render value= normally on subsequent loads. */
        bool slot_empty = (!t.arm && t.topic[0] == '\0' &&
                           t.label[0] == '\0' && t.minute_of_day == 0);
        char time_attr[40];
        if (slot_empty) {
            snprintf(time_attr, sizeof(time_attr), "required");
        } else {
            snprintf(time_attr, sizeof(time_attr), "value='%02u:%02u' required",
                     (unsigned)(t.minute_of_day / 60),
                     (unsigned)(t.minute_of_day % 60));
        }

        /* P1 #9: compute the human-readable next-fire string so the
         * user can sanity-check their schedule in context. Reflects
         * the device's POSIX TZ — catches misconfigured zones before
         * the user discovers the problem at the scheduled time. */
        char next_fire_str[64] = "";
        portal_format_next_fire(slot, next_fire_str, sizeof(next_fire_str));

        /* Pre-flight saved banner from ?saved=1 */
        bool saved = (strstr(req.query, "saved=1") != NULL);
        int pos = 0;
        if (saved) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<div class='st ok'>Saved.</div>");
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Timer %d&nbsp;</legend>"
            "<form id='timer-save' method='POST' action='/timers/save'>"
            "<input type='hidden' name='csrf' value='%s'>"
            "<input type='hidden' name='n' value='%d'>"
            "<label>Label</label>"
            "<input type='text' name='label' maxlength='%d' value='%s'>"
            "<p><label><input type='checkbox' name='arm' value='1'%s>Arm</label>"
            "<label><input type='checkbox' name='repeat' value='1'%s>Repeat</label></p>"
            /* P0 #2: dropped "24h" claim — <input type='time'> renders
             * AM/PM in US locales regardless of what the label says.
             * POST still carries 24h "HH:MM". */
            "<label>Time (local)</label>"
            "<input type='time' name='time' %s>"
            "<p style='color:#aaa;font-size:0.85em;margin:4px 0 8px'>"
            "Next fire: <b>%s</b></p>"
            "<label>Window (± minutes random jitter, 0–15)</label>"
            "<input type='number' name='window' min='0' max='15' value='%u'>"
            "<label>Days</label>"
            "<p style='text-align:left'>"
            "<label style='display:inline;margin-right:8px'><input type='checkbox' name='d0' value='1'%s>Sun</label>"
            "<label style='display:inline;margin-right:8px'><input type='checkbox' name='d1' value='1'%s>Mon</label>"
            "<label style='display:inline;margin-right:8px'><input type='checkbox' name='d2' value='1'%s>Tue</label>"
            "<label style='display:inline;margin-right:8px'><input type='checkbox' name='d3' value='1'%s>Wed</label>"
            "<label style='display:inline;margin-right:8px'><input type='checkbox' name='d4' value='1'%s>Thu</label>"
            "<label style='display:inline;margin-right:8px'><input type='checkbox' name='d5' value='1'%s>Fri</label>"
            "<label style='display:inline'><input type='checkbox' name='d6' value='1'%s>Sat</label>"
            "</p>"
            "<p style='font-size:0.85em;margin:4px 0'>"
            "<a href='/timers/edit?n=%d&preset=weekdays'>Weekdays</a> \xc2\xb7 "
            "<a href='/timers/edit?n=%d&preset=weekends'>Weekends</a> \xc2\xb7 "
            "<a href='/timers/edit?n=%d&preset=all'>Every day</a> \xc2\xb7 "
            "<a href='/timers/edit?n=%d&preset=none'>Clear</a></p>",
            slot,
            csrf_token_hex(), slot,
            TIMERS_LABEL_MAX, t.label,
            t.arm ? " checked" : "",
            t.repeat ? " checked" : "",
            time_attr, next_fire_str,
            (unsigned)t.window,
            (t.days & TIMERS_DAY_SUN) ? " checked" : "",
            (t.days & TIMERS_DAY_MON) ? " checked" : "",
            (t.days & TIMERS_DAY_TUE) ? " checked" : "",
            (t.days & TIMERS_DAY_WED) ? " checked" : "",
            (t.days & TIMERS_DAY_THU) ? " checked" : "",
            (t.days & TIMERS_DAY_FRI) ? " checked" : "",
            (t.days & TIMERS_DAY_SAT) ? " checked" : "",
            slot, slot, slot, slot);

        /* Apply preset day mask via query string redirect-friendly UX:
         * we just adjust the checkboxes inline via JS. (Server-side would
         * require a follow-up redirect; keep this lightweight.) */
        const char *preset = strstr(req.query, "preset=");
        if (preset) {
            preset += 7;
            const char *script = NULL;
            if (strncmp(preset, "weekdays", 8) == 0)
                script = "var s=[0,0,1,1,1,1,1,0];";
            else if (strncmp(preset, "weekends", 8) == 0)
                script = "var s=[0,1,0,0,0,0,0,1];";
            else if (strncmp(preset, "all", 3) == 0)
                script = "var s=[0,1,1,1,1,1,1,1];";
            else if (strncmp(preset, "none", 4) == 0)
                script = "var s=[0,0,0,0,0,0,0,0];";
            if (script) {
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<script>%s"
                    "for(var i=0;i<7;i++){var e=document.getElementsByName('d'+i)[0];if(e)e.checked=!!s[i+1];}"
                    "</script>", script);
            }
        }

        /* 0.8.2 P2 #10/#11: Save / Test fire / Clear share a flex row on
         * ≥ 600 px viewports. The wrapping <form id='timer-save'> closes
         * just before .tbtns; the Save button uses form='timer-save' so
         * it can submit it from outside. Three sibling <form>s sit inside
         * .tbtns (Save's wrapper is intentionally empty so flex spacing
         * stays uniform). Below 600 px the row collapses to stacked
         * full-width buttons (mobile). */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<style>"
            ".tbtns{display:flex;flex-direction:column;gap:6px;margin-top:8px}"
            ".tbtns form{flex:1;margin:0}"
            ".tbtns .btn{margin:0}"
            "@media (min-width:600px){.tbtns{flex-direction:row}}"
            "</style>"
            "<hr style='border:0;border-top:1px solid #555;margin:12px 0'>"
            "<label>Publish topic</label>"
            "<input type='text' name='topic' maxlength='%d' value='%s' placeholder='home/lights/cmd'>"
            "<label>Payload (≤ %d bytes, UTF-8)</label>"
            "<input type='text' name='payload' maxlength='%d' value='%.*s' placeholder='ON'>"
            "<p><label><input type='radio' name='qos' value='0'%s>QoS 0</label>"
            "<label><input type='radio' name='qos' value='1'%s>QoS 1</label>"
            "<label><input type='checkbox' name='retain' value='1'%s>Retain</label></p>"
            "</form>"
            "<div class='tbtns'>"
            "<form>"
            "<button form='timer-save' type='submit' class='btn bgrn'>Save</button>"
            "</form>"
            "<form method='POST' action='/timers/fire'>"
            "<input type='hidden' name='csrf' value='%s'>"
            "<input type='hidden' name='n' value='%d'>"
            "<button type='submit' class='btn'>Test fire now</button>"
            "</form>"
            "<form method='POST' action='/timers/clear' "
            "onsubmit=\"return confirm('Clear timer %d?')\">"
            "<input type='hidden' name='csrf' value='%s'>"
            "<input type='hidden' name='n' value='%d'>"
            "<button type='submit' class='btn bred'>Clear slot</button>"
            "</form>"
            "</div>"
            "</fieldset>"
            "<a href='/timers' class='btn bgry'>Back</a>",
            TIMERS_TOPIC_MAX, t.topic,
            TIMERS_PAYLOAD_MAX,
            TIMERS_PAYLOAD_MAX, (int)t.payload_len, t.payload,
            (t.qos == 0) ? " checked" : "",
            (t.qos == 1) ? " checked" : "",
            t.retain ? " checked" : "",
            csrf_token_hex(), slot,
            slot, csrf_token_hex(), slot);

        http_send_page(client_fd, body, (size_t)pos);

    } else if (strcmp(req.path, "/timers/save") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        char val[300];
        int slot = 0;
        if (urldecode_param(req.body, "n", val, sizeof(val))) slot = atoi(val);
        if (slot < 1 || slot > TIMERS_SLOT_COUNT) {
            http_send_plain(client_fd, "400 Bad Request", "bad slot");
            free(body); close(client_fd); return;
        }
        timer_slot_t t;
        memset(&t, 0, sizeof(t));
        t.arm    = (strstr(req.body, "arm=1") != NULL);
        t.repeat = (strstr(req.body, "repeat=1") != NULL);
        t.retain = (strstr(req.body, "retain=1") != NULL);
        if (urldecode_param(req.body, "qos", val, sizeof(val))) {
            int q = atoi(val); if (q == 0 || q == 1) t.qos = (uint8_t)q;
        }
        if (urldecode_param(req.body, "window", val, sizeof(val))) {
            int w = atoi(val);
            if (w >= 0 && w <= 15) t.window = (uint8_t)w;
        }
        if (urldecode_param(req.body, "time", val, sizeof(val))) {
            unsigned hh = 0, mm = 0;
            if (sscanf(val, "%u:%u", &hh, &mm) == 2 && hh < 24 && mm < 60) {
                t.minute_of_day = (uint16_t)(hh * 60 + mm);
            }
        }
        for (int d = 0; d < 7; d++) {
            char key[4]; snprintf(key, sizeof(key), "d%d", d);
            char dv[8] = "";
            urldecode_param(req.body, key, dv, sizeof(dv));
            if (dv[0] == '1') t.days |= (uint8_t)(1u << d);
        }
        if (urldecode_param(req.body, "topic", val, sizeof(val))) {
            strncpy(t.topic, val, TIMERS_TOPIC_MAX);
            t.topic[TIMERS_TOPIC_MAX] = '\0';
        }
        if (urldecode_param(req.body, "payload", val, sizeof(val))) {
            size_t pl = strnlen(val, TIMERS_PAYLOAD_MAX);
            memcpy(t.payload, val, pl);
            t.payload[pl] = '\0';
            t.payload_len = (uint16_t)pl;
        }
        if (urldecode_param(req.body, "label", val, sizeof(val))) {
            strncpy(t.label, val, TIMERS_LABEL_MAX);
            t.label[TIMERS_LABEL_MAX] = '\0';
        }
        char err[80] = "";
        esp_err_t setr = timers_set(slot, &t, err, sizeof(err));
        if (setr != ESP_OK) {
            int len = snprintf(body, PAGE_BUF_SIZE,
                "<fieldset><legend>&nbsp;Error&nbsp;</legend>"
                "<p style='color:#ff8888'>%s</p>"
                "<a href='/timers/edit?n=%d' class='btn'>Back</a></fieldset>",
                err[0] ? err : "validation failed", slot);
            http_send_page(client_fd, body, (size_t)len);
            free(body); close(client_fd); return;
        }
        /* 302 back to /timers */
        char loc[64];
        snprintf(loc, sizeof(loc), "/timers/edit?n=%d&saved=1", slot);
        int hlen = snprintf(body, PAGE_BUF_SIZE,
            "HTTP/1.1 302 Found\r\nLocation: %s\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n", loc);
        send(client_fd, body, (size_t)hlen, 0);
        free(body); close(client_fd); return;

    } else if (strcmp(req.path, "/timers/clear") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        char val[16];
        int slot = 0;
        if (urldecode_param(req.body, "n", val, sizeof(val))) slot = atoi(val);
        if (slot < 1 || slot > TIMERS_SLOT_COUNT) {
            http_send_plain(client_fd, "400 Bad Request", "bad slot");
            free(body); close(client_fd); return;
        }
        timers_clear(slot);
        int hlen = snprintf(body, PAGE_BUF_SIZE,
            "HTTP/1.1 302 Found\r\nLocation: /timers\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n");
        send(client_fd, body, (size_t)hlen, 0);
        free(body); close(client_fd); return;

    } else if (strcmp(req.path, "/timers/master") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        char val[8] = "";
        urldecode_param(req.body, "enable", val, sizeof(val));
        timers_set_master_enabled(val[0] == '1');
        int hlen = snprintf(body, PAGE_BUF_SIZE,
            "HTTP/1.1 302 Found\r\nLocation: /timers\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n");
        send(client_fd, body, (size_t)hlen, 0);
        free(body); close(client_fd); return;

    } else if (strcmp(req.path, "/timers/fire") == 0 && req.method == REQ_POST) {
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        char val[16];
        int slot = 0;
        if (urldecode_param(req.body, "n", val, sizeof(val))) slot = atoi(val);
        esp_err_t fr = timers_fire_now(slot);
        const char *loc = (fr == ESP_OK) ? "/timers?fired=1" : "/timers?fired=0";
        char redir_loc[64];
        snprintf(redir_loc, sizeof(redir_loc),
                 (fr == ESP_OK) ? "/timers/edit?n=%d&saved=1" : "/timers/edit?n=%d",
                 slot);
        if (slot >= 1 && slot <= TIMERS_SLOT_COUNT) loc = redir_loc;
        int hlen = snprintf(body, PAGE_BUF_SIZE,
            "HTTP/1.1 302 Found\r\nLocation: %s\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n", loc);
        send(client_fd, body, (size_t)hlen, 0);
        free(body); close(client_fd); return;

    } else if (strncmp(req.path, "/api/timers/", 12) == 0 &&
               (req.method == REQ_PUT || req.method == REQ_DELETE)) {
        /* 0.8.2: write half of the JSON API. Path is /api/timers/<n>
         * (1-based). CSRF is required for both methods. PUT body is
         * a single JSON slot object — same schema as one element of
         * GET /api/timers "timers" array. DELETE wipes the slot. */
        if (!csrf_verify(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
            free(body); close(client_fd); return;
        }
        int slot = atoi(req.path + 12);
        if (slot < 1 || slot > TIMERS_SLOT_COUNT) {
            const char *err = "{\"error\":\"slot out of range (1..16)\"}";
            size_t el = strlen(err);
            http_response_start(client_fd, "400 Bad Request", "application/json", el);
            http_send_body(client_fd, err, el);
            free(body); close(client_fd); return;
        }
        if (req.method == REQ_DELETE) {
            esp_err_t cr = timers_clear(slot);
            if (cr != ESP_OK) {
                const char *err = "{\"error\":\"clear failed\"}";
                size_t el = strlen(err);
                http_response_start(client_fd, "500 Internal Server Error",
                                    "application/json", el);
                http_send_body(client_fd, err, el);
            } else {
                char ok[64];
                int ol = snprintf(ok, sizeof(ok),
                    "{\"cleared\":true,\"n\":%d}", slot);
                http_response_start(client_fd, "200 OK", "application/json", (size_t)ol);
                http_send_body(client_fd, ok, (size_t)ol);
            }
            free(body); close(client_fd); return;
        }
        /* PUT: parse JSON body → timer_slot_t → validate → persist. */
        timer_slot_t newslot;
        if (timers_parse_slot_json(req.body, &newslot) != ESP_OK) {
            const char *err = "{\"error\":\"malformed JSON body\"}";
            size_t el = strlen(err);
            http_response_start(client_fd, "400 Bad Request", "application/json", el);
            http_send_body(client_fd, err, el);
            free(body); close(client_fd); return;
        }
        char verr[80] = "";
        esp_err_t sr = timers_set(slot, &newslot, verr, sizeof(verr));
        if (sr != ESP_OK) {
            char errjson[160];
            int el = snprintf(errjson, sizeof(errjson),
                "{\"error\":\"%s\"}",
                verr[0] ? verr : "validation failed");
            http_response_start(client_fd, "400 Bad Request",
                                "application/json", (size_t)el);
            http_send_body(client_fd, errjson, (size_t)el);
            free(body); close(client_fd); return;
        }
        int64_t nx = timers_next_fire_unix(slot);
        char ok[96];
        int ol = snprintf(ok, sizeof(ok),
            "{\"saved\":true,\"n\":%d,\"next_fire_unix\":%lld}",
            slot, (long long)nx);
        http_response_start(client_fd, "200 OK", "application/json", (size_t)ol);
        http_send_body(client_fd, ok, (size_t)ol);
        free(body); close(client_fd); return;

    } else if (strcmp(req.path, "/api/timers") == 0 && req.method == REQ_GET) {
        int pos = 0;
        time_t now_utc = time(NULL);
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "{\"schema\":1,\"master\":%s,\"now_unix\":%lld,\"dropped\":%u,\"timers\":[",
            timers_master_enabled() ? "true" : "false",
            (long long)now_utc, (unsigned)timers_dropped_count());
        for (int i = 1; i <= TIMERS_SLOT_COUNT; i++) {
            timer_slot_t t;
            timers_get(i, &t);
            char days_str[8];
            timers_days_to_string(t.days, days_str);
            int64_t nx = timers_next_fire_unix(i);
            if (i > 1 && pos < (int)PAGE_BUF_SIZE - 4) body[pos++] = ',';
            /* Inline JSON-escape macro for topic/label/payload. Handles
             * ", \, and control chars (validator already rejects most). */
            #define ESC_TO(src, srclen) do { \
                if (pos + 1 < PAGE_BUF_SIZE) body[pos++] = '"'; \
                for (size_t _i = 0; _i < (size_t)(srclen); _i++) { \
                    if (pos + 6 >= PAGE_BUF_SIZE) break; \
                    unsigned char _c = (unsigned char)(src)[_i]; \
                    if (_c == '"')       { body[pos++] = '\\'; body[pos++] = '"'; } \
                    else if (_c == '\\') { body[pos++] = '\\'; body[pos++] = '\\'; } \
                    else if (_c < 0x20)  { pos += snprintf(body+pos, PAGE_BUF_SIZE-pos, "\\u%04x", _c); } \
                    else                 { body[pos++] = (char)_c; } \
                } \
                if (pos + 1 < PAGE_BUF_SIZE) body[pos++] = '"'; \
            } while (0)
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "{\"n\":%d,\"arm\":%s,\"repeat\":%s,\"retain\":%s,\"qos\":%u,"
                "\"window\":%u,\"time\":\"%02u:%02u\",\"days\":\"%s\","
                "\"topic\":",
                i,
                t.arm ? "true" : "false",
                t.repeat ? "true" : "false",
                t.retain ? "true" : "false",
                (unsigned)t.qos,
                (unsigned)t.window,
                (unsigned)(t.minute_of_day / 60),
                (unsigned)(t.minute_of_day % 60),
                days_str);
            ESC_TO(t.topic, strnlen(t.topic, TIMERS_TOPIC_MAX));
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, ",\"label\":");
            ESC_TO(t.label, strnlen(t.label, TIMERS_LABEL_MAX));
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, ",\"payload\":");
            ESC_TO(t.payload, t.payload_len);
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                ",\"payload_len\":%u,\"next_fire_unix\":%lld}",
                (unsigned)t.payload_len, (long long)nx);
            #undef ESC_TO
            if (pos > PAGE_BUF_SIZE - 512) break;
        }
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos, "]}");
        http_response_start(client_fd, "200 OK", "application/json", (size_t)pos);
        http_send_body(client_fd, body, (size_t)pos);

    /* ============ Berry scripting ============
     * /berry/save and /berry/eval are handled by the streaming
     * intercept above (they don't reach this dispatch table). */
    } else if (strcmp(req.path, "/berry") == 0 && req.method == REQ_GET) {
        int pos = (int)portal_berry_render_page(body, PAGE_BUF_SIZE, csrf_token_hex());
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<br><a href='/' class='btn'>Main Menu</a>");
        http_send_page(client_fd, body, (size_t)pos);

    } else if (strcmp(req.path, "/berry/restart") == 0 && req.method == REQ_POST) {
        if (!portal_berry_do_restart(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
        } else {
            http_send_redirect(client_fd, "/berry?restarted=1");
        }

    } else if (strcmp(req.path, "/berry/enable") == 0 && req.method == REQ_POST) {
        char val[8] = "";
        bool en = false;
        if (urldecode_param(req.body, "enable", val, sizeof(val))) {
            en = (val[0] == '1');
        }
        if (!portal_berry_do_set_enabled(req.csrf_header, req.body, en)) {
            http_send_csrf_403(client_fd);
        } else {
            http_send_redirect(client_fd, "/berry?enabled=1");
        }

    } else if (strcmp(req.path, "/api/berry/status") == 0 && req.method == REQ_GET) {
        int pos = (int)portal_berry_render_status_json(body, PAGE_BUF_SIZE);
        http_response_start(client_fd, "200 OK", "application/json", (size_t)pos);
        http_send_body(client_fd, body, (size_t)pos);

    } else if (strcmp(req.path, "/api/berry/log") == 0 && req.method == REQ_GET) {
        int pos = (int)portal_berry_render_log_text(body, PAGE_BUF_SIZE);
        http_response_start(client_fd, "200 OK", "text/plain; charset=utf-8", (size_t)pos);
        http_send_body(client_fd, body, (size_t)pos);

    } else if (strcmp(req.path, "/api/berry/restart") == 0 && req.method == REQ_POST) {
        if (!portal_berry_do_restart(req.csrf_header, req.body)) {
            http_send_csrf_403(client_fd);
        } else {
            const char *r = "{\"ok\":true}";
            http_response_start(client_fd, "200 OK", "application/json", strlen(r));
            http_send_body(client_fd, r, strlen(r));
        }

    /* ============ 404 ============ */
    } else {
        int len = snprintf(body, PAGE_BUF_SIZE,
            "<!DOCTYPE html><html><head><title>404</title></head>"
            "<body style='font-family:sans-serif;padding:40px'>"
            "<h1>404 - Not Found</h1>"
            "<p>The requested page does not exist.</p>"
            "<a href='/'>Back to status</a></body></html>");
        http_response_start(client_fd, "404 Not Found", "text/html; charset=utf-8", len);
        http_send_body(client_fd, body, len);
    }

    int64_t elapsed_ms = (esp_timer_get_time() - req_start_us) / 1000;
    /* Access-logging policy:
     *   - Fast (<25ms):  ESP_LOGD only -- compiled out at the default log
     *                    verbosity (Info). Each console write adds ~5-10ms
     *                    of latency to the request itself (USB Serial JTAG
     *                    pushes synchronously through a small ring buf),
     *                    so logging every fast request would *inflate* the
     *                    very metric we're measuring. Enable via menuconfig
     *                    -> Component log verbosity -> Debug for full
     *                    per-request tracing during development.
     *   - Slow (>=25ms): ESP_LOGW. 25ms is just above the fast-path baseline
     *                    measured on 0.6.5 without the log line in the way;
     *                    anything slower is worth flagging in the field.
     * See docs/portal-latency-analysis.md. */
    if (elapsed_ms >= 25) {
        ESP_LOGW(TAG, "http  %s %s  done  %lldms  (slow)",
                 req_method, req_path, elapsed_ms);
    } else {
        ESP_LOGD(TAG, "http  %s %s  done  %lldms",
                 req_method, req_path, elapsed_ms);
    }

    free(body);
    close(client_fd);
}

static void portal_http_task(void *arg)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_http_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_http_fd < 0) {
        ESP_LOGE(TAG, "Failed to create HTTP socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_http_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "HTTP bind failed: %d", errno);
        close(s_http_fd);
        vTaskDelete(NULL);
        return;
    }

    /* Backlog 8 (was 4): browsers routinely open 6+ parallel connections
     * to one origin (the dashboard + /api/clients polling + the WS
     * endpoint + a second tab can easily get there). With backlog 4 the
     * 5th and 6th SYNs are dropped by LwIP and the user perceives 1-3s
     * connection hangs as the browser retries. MQTT broker's listen()
     * already uses 8 -- now consistent. */
    listen(s_http_fd, 8);
    ESP_LOGI(TAG, "Portal HTTP listening on port %d", PORTAL_HTTP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(s_http_fd, (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        handle_http_client(client_fd);
    }
}

static void portal_dns_task(void *arg)
{
    static uint8_t dns_resp[64];

    /* Get the current AP IP for DNS responses */
    char ap_ip_str[16] = "";
    wifi_get_ap_ip_str(ap_ip_str, sizeof(ap_ip_str));
    uint32_t ap_ip_addr = 0;
    {
        /* Parse IP octets from string */
        unsigned int a, b, c, d;
        if (sscanf(ap_ip_str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ap_ip_addr = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                         ((uint32_t)c << 8) | (uint32_t)d;
        }
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_dns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_fd < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_dns_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_dns_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno);
        close(s_dns_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack listening on port %d", PORTAL_DNS_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        uint8_t buf[512];

        ssize_t n = recvfrom(s_dns_fd, buf, sizeof(buf), 0,
            (struct sockaddr *)&client_addr, &addr_len);

        if (n < 12) continue;

        memcpy(dns_resp, buf, 12);
        dns_resp[2] = 0x81;
        dns_resp[4] = 0;
        dns_resp[5] = 1;

        int pos = 12;
        dns_resp[pos++] = 0xc0; dns_resp[pos++] = 0x0c;
        dns_resp[pos++] = 0;    dns_resp[pos++] = 1;
        dns_resp[pos++] = 0;    dns_resp[pos++] = 1;
        dns_resp[pos++] = 0;    dns_resp[pos++] = 0;
        dns_resp[pos++] = 0;    dns_resp[pos++] = 4;
        dns_resp[pos++] = (ap_ip_addr >> 24) & 0xFF;
        dns_resp[pos++] = (ap_ip_addr >> 16) & 0xFF;
        dns_resp[pos++] = (ap_ip_addr >> 8) & 0xFF;
        dns_resp[pos++] = ap_ip_addr & 0xFF;

        sendto(s_dns_fd, dns_resp, (size_t)pos, 0,
            (struct sockaddr *)&client_addr, addr_len);
    }
}

void portal_start(void)
{
    portal_load_wifi_state();

    /* Pin both portal tasks to CPU 0.
     *
     * Before: xTaskCreate() => no affinity, FreeRTOS schedules on whichever
     * core has room. Frequently landed on CPU 1 at equal priority to the
     * MQTT broker task (also priority 5, pinned to CPU 1), causing 10ms
     * tick round-robin slices and ~100ms p95 latency tails when the
     * broker was in a busy iteration (fanout, retained scan, QoS-1
     * retries).
     *
     * After: portal lives on CPU 0 alongside WiFi/LwIP/main/esp_timer --
     * all of which run at higher priorities (18-23) and pre-empt the
     * portal cleanly when they need the CPU. The broker keeps CPU 1 to
     * itself. This is the 'dedicated core' property users intuitively
     * expected. See docs/portal-latency-analysis.md for the measurements
     * that motivated this change.
     *
     * Per-WS tasks (portal_ws.c) are pinned the same way for the same
     * reason. */
    xTaskCreatePinnedToCore(portal_http_task, "portal_http",
                            PORTAL_TASK_STACK, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(portal_dns_task, "portal_dns",
                            PORTAL_TASK_STACK, NULL, 5, NULL, 0);

    char ap_ip[16] = "";
    wifi_get_ap_ip_str(ap_ip, sizeof(ap_ip));
    ESP_LOGI(TAG, "Captive portal started — AP IP: %s", ap_ip);
}

void portal_stop(void)
{
    if (s_dns_fd >= 0) { close(s_dns_fd); s_dns_fd = -1; }
    if (s_http_fd >= 0) { close(s_http_fd); s_http_fd = -1; }
    ESP_LOGI(TAG, "Captive portal stopped");
}
