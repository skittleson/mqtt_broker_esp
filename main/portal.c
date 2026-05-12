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
#include "version.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

/* ---- HTTP Request Parser ---- */

typedef enum {
    REQ_GET,
    REQ_POST
} http_method_t;

typedef struct {
    http_method_t method;
    char path[128];
    char body[512];
    char auth_user[65];   /* decoded from Authorization: Basic header */
    char auth_pass[65];
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
    if (qmark) *qmark = '\0';

    strncpy(req->path, path, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';

    char *body_start = strstr(space + 1, "\r\n\r\n");
    if (body_start && req->method == REQ_POST) {
        body_start += 4;
        size_t body_len = strlen(body_start);
        if (body_len > sizeof(req->body) - 1) {
            body_len = sizeof(req->body) - 1;
        }
        strncpy(req->body, body_start, body_len);
        req->body[body_len] = '\0';
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
    char header[256];
    int len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        status, content_type, (unsigned long)body_len);

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
        FW_NAME " " FW_VERSION "</div>"
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

/*
 * Handle multipart/form-data OTA upload.
 * Called when we detect POST /ota-upload in the initial header recv.
 * `header_buf` contains the first chunk already read (including the HTTP headers
 * and possibly the start of the body).  `header_len` is how many bytes are in it.
 */
static void handle_ota_upload(int client_fd, char *header_buf, int header_len)
{
    ESP_LOGI(TAG, "OTA upload starting");

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

    /* Send redirect page then reboot */
    #define PAGE_BUF_SIZE_OTA 512
    char *page = (char *)malloc(PAGE_BUF_SIZE_OTA);
    if (page) {
        int len = snprintf(page, PAGE_BUF_SIZE_OTA,
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>OTA Complete</title></head>"
            "<body style='font-family:sans-serif;padding:40px;text-align:center;"
            "background:#252525;color:#eaeaea'>"
            "<h2>Firmware Updated!</h2>"
            "<p>Downloaded %zu bytes. Rebooting...</p>"
            "<p>Reconnect in a few seconds.</p>"
            "</body></html>", total);
        http_response_start(client_fd, "200 OK", "text/html; charset=utf-8", len);
        http_send_body(client_fd, page, len);
        free(page);
    }
    close(client_fd);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ---- HTTP Handlers ---- */

static void handle_http_client(int client_fd)
{
    char *buf = (char *)malloc(2048);
    if (!buf) { close(client_fd); return; }
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

    if (total <= 0) { free(buf); close(client_fd); return; }

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

    http_request_t req;
    http_parse((const uint8_t *)buf, (size_t)total, &req);
    free(buf);  /* done with recv buffer */

    /* Allocate a shared page-body buffer on the heap */
    #define PAGE_BUF_SIZE 16384
    char *body = (char *)malloc(PAGE_BUF_SIZE);
    if (!body) { close(client_fd); return; }

    /* ---- HTTP Basic Auth check ---- */
    /* Uses the same auth_user / auth_pass as MQTT broker auth.
     * If both are empty in NVS, auth is disabled (open portal). */
    {
        char cfg_user[65] = "";
        char cfg_pass[65] = "";
        nvs_settings_get_str("auth_user", cfg_user, sizeof(cfg_user), "");
        nvs_settings_get_str("auth_pass", cfg_pass, sizeof(cfg_pass), "");

        if (cfg_user[0] != '\0' && cfg_pass[0] != '\0') {
            /* Auth is enabled — check credentials */
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
                free(body);
                close(client_fd);
                return;
            }
        }
    }

    /* ============ MAIN STATUS PAGE ============ */
    if (strcmp(req.path, "/") == 0 || strcmp(req.path, "/index.html") == 0) {
        int pos = 0;

        /* WiFi status banner */
        int sta_connected = 0, ap_running = 0;
        char ip_str[16] = "";
        char ssid_str[33] = "";
        wifi_get_ap_ip_str(ip_str, sizeof(ip_str));  /* default to AP IP */
        portal_get_sta_status(&sta_connected, &ap_running, ip_str, sizeof(ip_str),
                               ssid_str, sizeof(ssid_str));

        if (sta_connected) {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<div class='st ok'>WiFi connected — %s (%s)</div>", ssid_str, ip_str);
        } else {
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "<div class='st warn'>AP mode — connect to configure WiFi</div>");
        }

#ifdef CONFIG_MQTT_BROKER_ETHERNET
        {
            char eth_ip[16] = "";
            int eth_up = eth_is_connected();
            if (eth_up) {
                eth_get_ip_str(eth_ip, sizeof(eth_ip));
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<div class='st ok'>Ethernet — %s (%s)</div>",
                    eth_ip, eth_napt_is_enabled() ? "NAPT on" : "NAPT off");
            } else {
                pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                    "<div class='st warn'>Ethernet — disconnected</div>");
            }
        }
#endif

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

        /* Tasmota-style navigation buttons */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<br>"
            "<a href='/config' class='btn'>Configure WiFi</a>"
            "<a href='/settings' class='btn'>Configuration</a>"
            "<a href='/clients' class='btn'>Connected Clients</a>"
            "<a href='/information' class='btn'>Information</a>"
            "<a href='/update' class='btn bgry'>Firmware Upgrade</a>"
            "<a href='/reboot' class='btn bred' "
            "onclick=\"return confirm('Restart?')\">Restart</a>");

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

        /* AP config */
        char ap_ssid[33] = "mqtt-broker";
        char ap_pass[65] = "mqtt1234";
        char ap_ip_info[16] = "";
        nvs_settings_get_str("ap_ssid", ap_ssid, sizeof(ap_ssid), "mqtt-broker");
        nvs_settings_get_str("ap_pass", ap_pass, sizeof(ap_pass), "mqtt1234");
        wifi_get_ap_ip_str(ap_ip_info, sizeof(ap_ip_info));
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Access Point&nbsp;</legend><table>"
            "<tr><th>AP SSID</th><td>%s</td></tr>"
            "<tr><th>AP Password</th><td>%s</td></tr>"
            "<tr><th>AP IP</th><td>%s</td></tr>"
            "</table></fieldset>",
            ap_ssid, ap_pass, ap_ip_info);

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
            "<tr><th>Firmware</th><td>" FW_NAME " " FW_VERSION "</td></tr>"
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

        /* Load current settings */
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

        /* Device / network identity */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Device&nbsp;</legend>"
            "<form method='POST' action='/save-settings'>"
            "<label>Hostname (requires reboot)</label>"
            "<input type='text' name='hostname' value='%s' "
            "pattern='[A-Za-z0-9_\\-]{1,32}' maxlength='32' required "
            "title='1-32 chars: letters, digits, hyphen, underscore'>"
            "</fieldset>",
            hostname);

        /* MQTT settings */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;MQTT Broker&nbsp;</legend>"
            "<label>MQTT Port</label>"
            "<input type='number' name='mqtt_port' value='%u' min='1' max='65535'>"
            "<label>Auth Username (blank = disabled)</label>"
            "<input type='text' name='auth_user' value='%s' placeholder='Leave empty to disable'>"
            "<label>Auth Password</label>"
            "<input type='password' name='auth_pass' value='%s' placeholder='Password'>"
            "<label>Buffer Size (bytes, recv and send)</label>"
            "<input type='number' name='buf_size' value='%u' min='1024' max='65536' step='1024'>",
            mqtt_port, auth_user, auth_pass, buf_size);

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

        /* AP settings in same form */
        {
            char ap_ip_val[16] = "";
            wifi_get_ap_ip_str(ap_ip_val, sizeof(ap_ip_val));
            pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
                "</fieldset>"
                "<fieldset><legend>&nbsp;Access Point&nbsp;</legend>"
                "<label>AP SSID</label>"
                "<input type='text' name='ap_ssid' value='%s' placeholder='mqtt-broker' required maxlength='32'>"
                "<label>AP Password (min 8 chars)</label>"
                "<input type='password' name='ap_pass' value='%s' placeholder='mqtt1234' "
                "minlength='8' maxlength='63' required "
                "title='AP password must be 8-63 characters'>"
                "<label>AP IP Address (requires reboot)</label>"
                "<input type='text' name='ap_ip' value='%s' "
                "pattern='[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+' "
                "title='IPv4 address like 192.168.25.1'>",
                ap_ssid, ap_pass, ap_ip_val);
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

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "</fieldset>"
            "<br><button type='submit' class='bgrn'>Save</button>"
            "</form>"
            "<br><a href='/' class='btn'>Main Menu</a>");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ SAVE SETTINGS ============ */
    } else if (strcmp(req.path, "/save-settings") == 0 && req.method == REQ_POST) {
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
            nvs_settings_set_str("auth_pass", val);
            ESP_LOGI(TAG, "Saved auth pass (len=%d)", (int)strlen(val));
        }
        if (urldecode_param(req.body, "ap_ssid", val, sizeof(val))) {
            if (val[0]) {
                nvs_settings_set_str("ap_ssid", val);
                ESP_LOGI(TAG, "Saved AP SSID: '%s'", val);
            }
        }
        if (urldecode_param(req.body, "ap_pass", val, sizeof(val))) {
            if (strlen(val) >= 8) {
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
        /* NAPT toggle — checkbox absent means unchecked (disabled).
         * Apply immediately without reboot. */
        {
            uint8_t napt_en = (strstr(req.body, "napt_en=1") != NULL) ? 1 : 0;
            uint8_t napt_was = nvs_settings_get_u8("napt_en", 1);
            nvs_settings_set_u8("napt_en", napt_en);

            if (napt_en != napt_was && eth_is_connected()) {
                if (napt_en) {
                    eth_napt_enable();
                } else {
                    eth_napt_disable();
                }
            }
            ESP_LOGI(TAG, "Saved NAPT enabled: %d", napt_en);
        }
#endif

        http_send_redirect(client_fd, "/settings");

    /* ============ WIFI CONFIG PAGE ============ */
    } else if (strcmp(req.path, "/config") == 0) {
        int pos = 0;

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;WiFi Configuration&nbsp;</legend>"
            "<form method='POST' action='/save'>"
            "<label>WiFi SSID</label>"
            "<input type='text' name='ssid' placeholder='Network name' required maxlength='32' autofocus>"
            "<label>WiFi Password</label>"
            "<input type='password' name='password' placeholder='Network password' "
            "minlength='8' maxlength='63' required "
            "title='WiFi password must be 8-63 characters'>"
            "<p><label style='font-weight:normal'>"
            "<input type='checkbox' name='ap_mode' value='1' %s> "
            "Enable AP mode alongside WiFi</label></p>"
            "<br><button type='submit' class='bgrn'>Save</button>"
            "</form></fieldset>"
            "<br><a href='/' class='btn'>Main Menu</a>",
            s_portal_ap_mode ? "checked" : "");

        http_send_page(client_fd, body, (size_t)pos);

    /* ============ SAVE WIFI ============ */
    } else if (strcmp(req.path, "/save") == 0 && req.method == REQ_POST) {
        char ssid[33] = "", password[65] = "";
        int ap_mode = 0;
        urldecode_param(req.body, "ssid", ssid, sizeof(ssid));
        urldecode_param(req.body, "password", password, sizeof(password));
        if (strstr(req.body, "ap_mode=1")) ap_mode = 1;

        if (ssid[0] == '\0' || strlen(password) < 8) {
            ESP_LOGW(TAG, "WiFi save rejected: SSID empty or password too short");
            http_send_redirect(client_fd, "/config");
        } else {
            ESP_LOGI(TAG, "Saving WiFi: SSID='%s', ap_mode=%d", ssid, ap_mode);
            portal_save_wifi(ssid, password, ap_mode);
            http_send_redirect(client_fd, "/");
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

    /* ============ REBOOT ============ */
    } else if (strcmp(req.path, "/reboot") == 0) {
        int len = snprintf(body, PAGE_BUF_SIZE,
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>Rebooting</title></head>"
            "<body style='font-family:sans-serif;padding:40px;text-align:center'>"
            "<h2>Rebooting...</h2>"
            "<p>The device will restart. Reconnect in a few seconds.</p>"
            "</body></html>");
        http_response_start(client_fd, "200 OK", "text/html; charset=utf-8", len);
        http_send_body(client_fd, body, len);
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
                "<th style='width:22%%'>Client ID</th>"
                "<th style='width:18%%'>IP Address</th>"
                "<th style='width:13%%'>Connected</th>"
                "<th style='width:13%%'>Last Active</th>"
                "<th style='width:8%%' title='Active subscriptions'>Subs</th>"
                "<th style='width:13%%' title='PUBLISH packets accepted from this client since it connected'>Published</th>"
                "<th style='width:13%%'>Keep-Alive</th>"
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
                    "<td>%d</td><td>%lu</td><td>%us</td></tr>",
                    clients[i].client_id[0] ? clients[i].client_id : "<em>(empty)</em>",
                    clients[i].ip, clients[i].ip,
                    conn_h, conn_m, conn_s,
                    last_sec,
                    clients[i].subscriptions,
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

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<p style='color:#888;font-size:0.85em;text-align:center'>"
            "Page auto-refreshes every 5 seconds</p>"
            "<script>setTimeout(function(){location.reload();},5000);</script>"
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
                "\"subs\":%d,\"published\":%lu,\"keep_alive\":%u}",
                clients[i].client_id, clients[i].ip,
                (long long)(clients[i].connected_ms / 1000),
                (long long)(clients[i].last_active_ms / 1000),
                clients[i].subscriptions,
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

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Firmware Information&nbsp;</legend><table>"
            "<tr><th>Current Version</th><td>" FW_VERSION "</td></tr>"
            "<tr><th>Build Date</th><td>" __DATE__ " " __TIME__ "</td></tr>"
            "<tr><th>IDF Version</th><td>%s</td></tr>"
            "<tr><th>Running Partition</th><td>%s</td></tr>"
            "</table></fieldset>",
            app->idf_ver,
            running ? running->label : "unknown");

        /* File upload form */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;Upload Firmware&nbsp;</legend>"
            "<form method='POST' action='/ota-upload' enctype='multipart/form-data' "
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
            "\"<div class='st ok'>Upload complete! Rebooting...</div>\";"
            "setTimeout(function(){location.href='/';},10000);"
            "}else{"
            "document.getElementById('fwprog').innerHTML="
            "\"<div class='st warn'>Upload failed: \"+x.responseText+\"</div>\";"
            "document.getElementById('fwbtn').disabled=false;}};"
            "x.onerror=function(){"
            "document.getElementById('fwprog').innerHTML="
            "\"<div class='st warn'>Connection error</div>\";"
            "document.getElementById('fwbtn').disabled=false;};"
            "var fd=new FormData();fd.append('firmware',f);"
            "x.open('POST','/ota-upload');"
            "x.send(fd);"
            "};"
            "</script>"
            "</fieldset>");

        /* URL-based OTA */
        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
            "<fieldset><legend>&nbsp;OTA Update via URL&nbsp;</legend>"
            "<form method='POST' action='/ota-url' id='urlform'>"
            "<label>Firmware URL (http:// only)</label>"
            "<input type='url' name='url' placeholder='http://192.168.1.100:8080/firmware.bin' "
            "required pattern='http://.*'>"
            "<br><button type='submit' class='bgrn'>Download &amp; Flash</button>"
            "</form></fieldset>");

        pos += snprintf(body + pos, PAGE_BUF_SIZE - pos,
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

    /* ============ OTA VIA URL ============ */
    } else if (strcmp(req.path, "/ota-url") == 0 && req.method == REQ_POST) {
        char url[256] = "";
        urldecode_param(req.body, "url", url, sizeof(url));

        if (url[0] == '\0' || strncmp(url, "http://", 7) != 0) {
            int len = snprintf(body, PAGE_BUF_SIZE,
                "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta http-equiv='refresh' content='3;url=/update'>"
                "</head><body style='font-family:sans-serif;padding:40px;text-align:center;"
                "background:#252525;color:#eaeaea'>"
                "<h2>Invalid URL</h2><p>Must start with http://</p>"
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

    listen(s_http_fd, 4);
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

    xTaskCreate(portal_http_task, "portal_http", PORTAL_TASK_STACK, NULL, 5, NULL);
    xTaskCreate(portal_dns_task, "portal_dns", PORTAL_TASK_STACK, NULL, 5, NULL);

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
