/*
 * portal_ws.c — RFC 6455 WebSocket endpoint for the MQTT topic tester.
 *
 * Design constraints (see plan-mqtt-ux.md):
 *   - Single accept loop in portal.c must not be blocked: this module spawns
 *     a dedicated task per WS client.
 *   - Hard cap on concurrent WS clients enforced by broker_tester_register().
 *   - No fragmented frames. No 64-bit lengths. Max 8 KB inbound payload.
 *   - JSON wire format only: text frames in both directions.
 *   - All recv/send/malloc return values checked; single cleanup path per task.
 */

#include "portal_ws.h"

#include "mqtt_broker.h"

#if !BROKER_TESTER_ENABLED

/* Tester disabled at compile time: provide a stub that 404s any WS upgrade. */
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
bool portal_ws_handle_upgrade(int fd, const char *raw_headers, size_t raw_len)
{
    (void)raw_headers; (void)raw_len;
    const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(fd, r, strlen(r), MSG_NOSIGNAL);
    close(fd);
    return false;
}

#else  /* BROKER_TESTER_ENABLED */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "esp_log.h"

static const char *TAG = "portal_ws";

/* ---- Tunables ---- */

#define WS_RX_BUF_SIZE      2048    /* max inbound frame including header */
#define WS_TX_BUF_SIZE      1024    /* max outbound JSON message */
#define WS_TASK_STACK       6144    /* generous; we cannot profile on live device */
#define WS_TASK_PRIO        4       /* < broker (5) */
#define WS_IDLE_TIMEOUT_MS  (5 * 60 * 1000)
#define WS_SB_CAPACITY      (16 * sizeof(broker_tester_event_t))  /* ~16 msgs queued */
#define WS_RATE_PUB_PER_SEC 20
#define WS_RATE_WINDOW_MS   1000

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* RFC 6455 §1.3 magic GUID */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ---- Per-task context ---- */

typedef struct {
    int                  fd;
    StreamBufferHandle_t sb;            /* events from broker */
    int                  broker_slot;   /* registration slot, -1 if not registered */
    uint8_t              rx[WS_RX_BUF_SIZE];
    char                 tx[WS_TX_BUF_SIZE];

    /* Rate limiting: simple sliding window. */
    int64_t              rate_window_start_ms;
    int                  rate_count;
} ws_ctx_t;

/* ---- Helpers ---- */

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Case-insensitive substring search bounded by haystack length. */
static const char *ci_strnstr(const char *hay, size_t haylen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > haylen) return NULL;
    for (size_t i = 0; i + nlen <= haylen; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (j == nlen) return hay + i;
    }
    return NULL;
}

/* Extract Sec-WebSocket-Key header value from raw HTTP request bytes.
 * Returns length copied to out, or 0 on failure. */
static size_t extract_ws_key(const char *raw, size_t raw_len, char *out, size_t out_size)
{
    const char *h = ci_strnstr(raw, raw_len, "Sec-WebSocket-Key:");
    if (!h) return 0;
    h += strlen("Sec-WebSocket-Key:");
    /* Skip leading spaces/tabs */
    while ((h < raw + raw_len) && (*h == ' ' || *h == '\t')) h++;
    /* Find end of line */
    const char *eol = h;
    while (eol < raw + raw_len && *eol != '\r' && *eol != '\n') eol++;
    size_t len = (size_t)(eol - h);
    if (len == 0 || len >= out_size) return 0;
    memcpy(out, h, len);
    out[len] = '\0';
    return len;
}

/* Compute the Sec-WebSocket-Accept value (base64(SHA1(key + GUID))). */
static bool compute_ws_accept(const char *key, char *out, size_t out_size)
{
    char concat[128];
    int n = snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
    if (n <= 0 || n >= (int)sizeof(concat)) return false;

    uint8_t sha[20];
    mbedtls_sha1((const unsigned char *)concat, (size_t)n, sha);

    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)out, out_size, &olen, sha, sizeof(sha));
    if (rc != 0) return false;
    if (olen >= out_size) return false;
    out[olen] = '\0';
    return true;
}

/* Send a small HTTP error response and close. Used during pre-handshake. */
static void send_http_error(int fd, const char *status, const char *body)
{
    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n%s",
        status, (unsigned)strlen(body), body);
    if (n > 0) send(fd, resp, (size_t)n, MSG_NOSIGNAL);
}

/* Blocking send-all on a non-fatal-EINTR socket. Returns true if all sent. */
static bool send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/* Build a server->client text frame in `out` (which must be at least len+10 bytes).
 * Returns total frame length, or -1 on overflow. */
static int build_text_frame(uint8_t *out, size_t out_cap, const char *payload, size_t len)
{
    if (len < 126) {
        if (out_cap < 2 + len) return -1;
        out[0] = 0x80 | WS_OP_TEXT;     /* FIN + text */
        out[1] = (uint8_t)len;          /* no mask bit */
        memcpy(out + 2, payload, len);
        return (int)(2 + len);
    } else if (len <= 0xFFFF) {
        if (out_cap < 4 + len) return -1;
        out[0] = 0x80 | WS_OP_TEXT;
        out[1] = 126;
        out[2] = (uint8_t)((len >> 8) & 0xFF);
        out[3] = (uint8_t)(len & 0xFF);
        memcpy(out + 4, payload, len);
        return (int)(4 + len);
    }
    return -1;  /* we never send anything that big */
}

static bool send_text(int fd, const char *payload, size_t len)
{
    uint8_t hdr[WS_TX_BUF_SIZE + 16];
    int total = build_text_frame(hdr, sizeof(hdr), payload, len);
    if (total < 0) return false;
    return send_all(fd, hdr, (size_t)total);
}

/* Send a CLOSE frame with status code. Best-effort. */
static void send_close(int fd, uint16_t code)
{
    uint8_t frame[4] = { 0x80 | WS_OP_CLOSE, 0x02,
                         (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
    send_all(fd, frame, sizeof(frame));
}

/* Send a PONG (no payload). */
static void send_pong(int fd, const uint8_t *payload, size_t len)
{
    uint8_t frame[2 + 125];
    if (len > 125) len = 125;  /* control frames are limited */
    frame[0] = 0x80 | WS_OP_PONG;
    frame[1] = (uint8_t)len;
    if (len > 0) memcpy(frame + 2, payload, len);
    send_all(fd, frame, 2 + len);
}

/* Read exactly n bytes from fd into buf, or fail. Uses select() for timeout. */
static bool recv_n(int fd, uint8_t *buf, size_t n, int timeout_ms)
{
    size_t got = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (got < n) {
        int64_t remain = deadline - now_ms();
        if (remain <= 0) return false;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = (long)(remain / 1000),
                              .tv_usec = (long)((remain % 1000) * 1000) };
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (sel == 0) return false;  /* timeout */

        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;  /* peer closed */
        got += (size_t)r;
    }
    return true;
}

/* Receive one full WebSocket frame.
 * Returns:
 *   >= 0 : opcode; payload (after unmasking) placed in ctx->rx, len in *payload_len
 *   -1   : socket error / peer closed / protocol error
 * If a CLOSE frame is received this returns WS_OP_CLOSE and the caller should
 * shut down. Control frames (ping/pong) are handled inline by signalling
 * opcode back to caller, which dispatches.
 */
static int recv_frame(ws_ctx_t *ctx, size_t *payload_len, int timeout_ms)
{
    uint8_t hdr[2];
    if (!recv_n(ctx->fd, hdr, 2, timeout_ms)) return -1;

    bool fin = (hdr[0] & 0x80) != 0;
    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;

    /* Per RFC 6455: client->server frames MUST be masked. */
    if (!masked) return -1;
    /* We don't implement fragmentation. */
    if (!fin) return -1;
    /* Reserved bits must be zero. */
    if (hdr[0] & 0x70) return -1;

    if (len == 126) {
        uint8_t ext[2];
        if (!recv_n(ctx->fd, ext, 2, timeout_ms)) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        /* Refuse huge frames. */
        return -1;
    }

    if (len > WS_RX_BUF_SIZE) return -1;

    uint8_t mask[4];
    if (!recv_n(ctx->fd, mask, 4, timeout_ms)) return -1;

    if (len > 0) {
        if (!recv_n(ctx->fd, ctx->rx, (size_t)len, timeout_ms)) return -1;
        for (size_t i = 0; i < len; i++) ctx->rx[i] ^= mask[i & 3];
    }

    *payload_len = (size_t)len;
    return opcode;
}

/* ---- Minimal JSON helpers ---- */

/* Skip whitespace. */
static const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Find a top-level "key":  in a flat JSON object. Returns pointer to the
 * character after the colon, or NULL. Skips over string values so a payload
 * that happens to contain the key name doesn't false-match. Only depth 1
 * (sufficient for our wire format). */
static const char *json_find_key(const char *buf, size_t len, const char *key)
{
    size_t klen = strlen(key);
    const char *p = buf;
    const char *end = buf + len;
    bool at_key = false;  /* true when the next string will be a key, not value */
    while (p < end) {
        char c = *p;
        if (c == '{' || c == ',') { at_key = true;  p++; continue; }
        if (c == ':')              { at_key = false; p++; continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p++; continue; }
        if (c == '"') {
            const char *str = p + 1;
            const char *q = str;
            while (q < end && *q != '"') {
                if (*q == '\\' && q + 1 < end) q += 2;
                else q++;
            }
            if (q >= end) return NULL;  /* unterminated */
            if (at_key) {
                size_t this_len = (size_t)(q - str);
                if (this_len == klen && memcmp(str, key, klen) == 0) {
                    const char *after = json_skip_ws(q + 1, end);
                    if (after < end && *after == ':') return after + 1;
                }
            }
            p = q + 1;
            continue;
        }
        /* Other token (number/bool/null) — advance one char. */
        p++;
    }
    return NULL;
}

/* Decode a JSON string starting at *p (which must point at the opening quote).
 * Writes UTF-8 bytes into out (up to out_size-1), null-terminates, and
 * advances *p past the closing quote. Returns length written, or -1 on error.
 * Only handles common escapes: \" \\ \/ \b \f \n \r \t and \uXXXX (BMP, ascii-safe). */
static int json_decode_string(const char **pp, const char *end, char *out, size_t out_size)
{
    const char *p = *pp;
    if (p >= end || *p != '"') return -1;
    p++;
    size_t o = 0;
    while (p < end && *p != '"') {
        if (o + 1 >= out_size) return -1;
        if (*p == '\\') {
            if (p + 1 >= end) return -1;
            char c = p[1];
            switch (c) {
                case '"': case '\\': case '/': out[o++] = c; p += 2; break;
                case 'b': out[o++] = '\b'; p += 2; break;
                case 'f': out[o++] = '\f'; p += 2; break;
                case 'n': out[o++] = '\n'; p += 2; break;
                case 'r': out[o++] = '\r'; p += 2; break;
                case 't': out[o++] = '\t'; p += 2; break;
                case 'u': {
                    if (p + 6 > end) return -1;
                    unsigned u = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p[2 + i];
                        u <<= 4;
                        if (h >= '0' && h <= '9') u |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') u |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') u |= (unsigned)(h - 'A' + 10);
                        else return -1;
                    }
                    /* Encode as UTF-8. */
                    if (u < 0x80) {
                        if (o + 1 >= out_size) return -1;
                        out[o++] = (char)u;
                    } else if (u < 0x800) {
                        if (o + 2 >= out_size) return -1;
                        out[o++] = (char)(0xC0 | (u >> 6));
                        out[o++] = (char)(0x80 | (u & 0x3F));
                    } else {
                        if (o + 3 >= out_size) return -1;
                        out[o++] = (char)(0xE0 | (u >> 12));
                        out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (u & 0x3F));
                    }
                    p += 6;
                    break;
                }
                default: return -1;
            }
        } else {
            out[o++] = *p++;
        }
    }
    if (p >= end || *p != '"') return -1;
    p++;
    out[o] = '\0';
    *pp = p;
    return (int)o;
}

/* Parse "true" or "false". Returns 1/0, or -1 on error. */
static int json_parse_bool(const char *p, const char *end)
{
    p = json_skip_ws(p, end);
    if (end - p >= 4 && strncmp(p, "true", 4) == 0) return 1;
    if (end - p >= 5 && strncmp(p, "false", 5) == 0) return 0;
    return -1;
}

/* JSON-encode a string into out. Returns bytes written or -1 on overflow.
 * Escapes the mandatory characters; non-printable bytes become \u00XX. */
static int json_encode_string(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t c = in[i];
        if (o + 7 >= out_size) return -1;  /* worst case 6 chars + NUL */
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\b': out[o++] = '\\'; out[o++] = 'b'; break;
            case '\f': out[o++] = '\\'; out[o++] = 'f'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            default:
                if (c < 0x20) {
                    int n = snprintf(out + o, out_size - o, "\\u%04x", c);
                    if (n < 0 || (size_t)n >= out_size - o) return -1;
                    o += (size_t)n;
                } else {
                    out[o++] = (char)c;
                }
                break;
        }
    }
    if (o >= out_size) return -1;
    out[o] = '\0';
    return (int)o;
}

/* ---- Inbound publish handling ---- */

static void handle_publish_request(ws_ctx_t *ctx, const char *json, size_t json_len)
{
    /* Rate limit check. */
    int64_t now = now_ms();
    if (now - ctx->rate_window_start_ms >= WS_RATE_WINDOW_MS) {
        ctx->rate_window_start_ms = now;
        ctx->rate_count = 0;
    }
    if (ctx->rate_count >= WS_RATE_PUB_PER_SEC) {
        const char *err = "{\"error\":\"rate_limit\"}";
        send_text(ctx->fd, err, strlen(err));
        return;
    }
    ctx->rate_count++;

    const char *end = json + json_len;

    char topic[BROKER_TESTER_MAX_TOPIC_LEN + 1];
    char payload[BROKER_TESTER_MAX_PAYLOAD_LEN + 1];
    int  topic_len = 0;
    int  payload_len = 0;
    bool retain = false;

    const char *p = json_find_key(json, json_len, "topic");
    if (!p) { send_text(ctx->fd, "{\"error\":\"missing_topic\"}", 24); return; }
    p = json_skip_ws(p, end);
    topic_len = json_decode_string(&p, end, topic, sizeof(topic));
    if (topic_len <= 0) { send_text(ctx->fd, "{\"error\":\"bad_topic\"}", 20); return; }

    p = json_find_key(json, json_len, "payload");
    if (p) {
        p = json_skip_ws(p, end);
        payload_len = json_decode_string(&p, end, payload, sizeof(payload));
        if (payload_len < 0) {
            send_text(ctx->fd, "{\"error\":\"bad_payload\"}", 22);
            return;
        }
    }

    p = json_find_key(json, json_len, "retain");
    if (p) {
        int r = json_parse_bool(p, end);
        if (r >= 0) retain = (r != 0);
    }

    bool ok = broker_tester_request_publish(topic, (size_t)topic_len,
                                            (const uint8_t *)payload,
                                            (size_t)payload_len, retain);
    if (!ok) {
        send_text(ctx->fd, "{\"error\":\"queue_full_or_invalid\"}", 33);
    }
}

/* ---- Outbound event emission ---- */

static void emit_event(ws_ctx_t *ctx, const broker_tester_event_t *ev)
{
    /* Build JSON: {"t":"...","p":"...","r":0|1,"s":N,"trunc":true?} */
    /* Worst-case escaped sizes:
     *   topic: BROKER_TESTER_MAX_TOPIC_LEN * 6 = 768
     *   payload: BROKER_TESTER_MAX_PAYLOAD_LEN * 6 = 1536
     * WS_TX_BUF_SIZE=1024 isn't enough; bump to a stack buffer for emit. */
    char topic_enc[BROKER_TESTER_MAX_TOPIC_LEN * 6 + 4];
    char payload_enc[BROKER_TESTER_MAX_PAYLOAD_LEN * 6 + 4];

    int tn = json_encode_string((const uint8_t *)ev->topic, ev->topic_len,
                                topic_enc, sizeof(topic_enc));
    int pn = json_encode_string(ev->payload, ev->payload_len,
                                payload_enc, sizeof(payload_enc));
    if (tn < 0 || pn < 0) return;

    /* Final message buffer. */
    static char msg[3072];
    int n = snprintf(msg, sizeof(msg),
        "{\"t\":\"%s\",\"p\":\"%s\",\"r\":%d,\"s\":%u%s}",
        topic_enc, payload_enc, ev->retain ? 1 : 0,
        (unsigned)ev->seq, ev->truncated ? ",\"trunc\":true" : "");
    if (n <= 0 || n >= (int)sizeof(msg)) return;

    send_text(ctx->fd, msg, (size_t)n);
}

/* ---- Per-WS-client task ---- */

static void ws_task(void *arg)
{
    ws_ctx_t *ctx = (ws_ctx_t *)arg;
    int64_t last_activity = now_ms();

    /* Send a hello so the browser knows we're up. */
    send_text(ctx->fd, "{\"hello\":true}", 14);

    while (1) {
        /* Drain broker events first, non-blocking. */
        broker_tester_event_t ev;
        size_t got = xStreamBufferReceive(ctx->sb, &ev, sizeof(ev), 0);
        if (got == sizeof(ev)) {
            emit_event(ctx, &ev);
            last_activity = now_ms();
            continue;
        }

        /* Check for inbound data with a short timeout so we can revisit the
         * stream buffer frequently. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };  /* 100 ms */
        int sel = select(ctx->fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) {
            /* Idle timeout? */
            if (now_ms() - last_activity > WS_IDLE_TIMEOUT_MS) {
                send_close(ctx->fd, 1000);
                break;
            }
            continue;
        }

        size_t plen = 0;
        int op = recv_frame(ctx, &plen, 5000);
        if (op < 0) break;
        last_activity = now_ms();

        switch (op) {
            case WS_OP_TEXT:
                handle_publish_request(ctx, (const char *)ctx->rx, plen);
                break;
            case WS_OP_BIN:
                /* Not supported. */
                send_text(ctx->fd, "{\"error\":\"binary_unsupported\"}", 29);
                break;
            case WS_OP_PING:
                send_pong(ctx->fd, ctx->rx, plen);
                break;
            case WS_OP_PONG:
                /* Ignore. */
                break;
            case WS_OP_CLOSE:
                send_close(ctx->fd, 1000);
                goto done;
            default:
                /* Unknown opcode. */
                goto done;
        }
    }

done:
    if (ctx->broker_slot >= 0) broker_tester_unregister(ctx->broker_slot);
    if (ctx->sb) vStreamBufferDelete(ctx->sb);
    close(ctx->fd);
    free(ctx);
    ESP_LOGI(TAG, "ws task exiting");
    vTaskDelete(NULL);
}

/* ---- Public entry point ---- */

bool portal_ws_handle_upgrade(int fd, const char *raw_headers, size_t raw_len)
{
    /* Validate Upgrade + Connection headers. */
    if (!ci_strnstr(raw_headers, raw_len, "Upgrade: websocket") &&
        !ci_strnstr(raw_headers, raw_len, "Upgrade:websocket")) {
        send_http_error(fd, "400 Bad Request", "missing Upgrade: websocket");
        close(fd);
        return false;
    }
    if (!ci_strnstr(raw_headers, raw_len, "Connection:") ||
        !ci_strnstr(raw_headers, raw_len, "Upgrade")) {
        /* Loose check; browsers always set this. */
    }

    char key[64];
    if (extract_ws_key(raw_headers, raw_len, key, sizeof(key)) == 0) {
        send_http_error(fd, "400 Bad Request", "missing Sec-WebSocket-Key");
        close(fd);
        return false;
    }

    char accept[64];
    if (!compute_ws_accept(key, accept, sizeof(accept))) {
        send_http_error(fd, "500 Internal Server Error", "accept compute failed");
        close(fd);
        return false;
    }

    /* Reject early if broker says no room. */
    if (broker_tester_consumer_count() >= BROKER_TESTER_MAX_CONSUMERS) {
        send_http_error(fd, "503 Service Unavailable", "tester busy");
        close(fd);
        return false;
    }

    /* Allocate context + stream buffer BEFORE sending 101, so we can fail
     * with a clean HTTP error if we're out of RAM. */
    ws_ctx_t *ctx = (ws_ctx_t *)calloc(1, sizeof(ws_ctx_t));
    if (!ctx) {
        send_http_error(fd, "500 Internal Server Error", "out of memory");
        close(fd);
        return false;
    }
    ctx->fd = fd;
    ctx->broker_slot = -1;
    ctx->rate_window_start_ms = now_ms();

    ctx->sb = xStreamBufferCreate(WS_SB_CAPACITY, sizeof(broker_tester_event_t));
    if (!ctx->sb) {
        free(ctx);
        send_http_error(fd, "500 Internal Server Error", "stream buffer alloc failed");
        close(fd);
        return false;
    }

    ctx->broker_slot = broker_tester_register(ctx->sb);
    if (ctx->broker_slot < 0) {
        vStreamBufferDelete(ctx->sb);
        free(ctx);
        send_http_error(fd, "503 Service Unavailable", "tester busy");
        close(fd);
        return false;
    }

    /* Send 101 Switching Protocols. */
    char resp[256];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n <= 0 || !send_all(fd, (const uint8_t *)resp, (size_t)n)) {
        broker_tester_unregister(ctx->broker_slot);
        vStreamBufferDelete(ctx->sb);
        free(ctx);
        close(fd);
        return false;
    }

    /* Spawn the task; it now owns fd, sb, slot, and ctx.
     * Pinned to CPU 0 so it shares an affinity domain with portal_http
     * (also CPU 0 since 0.6.5). Keeps WS traffic off CPU 1 where the
     * MQTT broker runs. See docs/portal-latency-analysis.md. */
    BaseType_t rc = xTaskCreatePinnedToCore(ws_task, "portal_ws",
                                            WS_TASK_STACK, ctx,
                                            WS_TASK_PRIO, NULL, 0);
    if (rc != pdPASS) {
        broker_tester_unregister(ctx->broker_slot);
        vStreamBufferDelete(ctx->sb);
        free(ctx);
        send_close(fd, 1011);
        close(fd);
        return false;
    }

    ESP_LOGI(TAG, "WS upgraded (fd=%d, broker slot %d)", fd, ctx->broker_slot);
    return true;
}

#endif /* BROKER_TESTER_ENABLED */
