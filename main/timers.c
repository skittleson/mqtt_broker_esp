/*
 * Scheduled MQTT publishes — see timers.h for design notes.
 *
 * Wire format (compact JSON in NVS "mqtt_cfg"/"timers", ≤ 5 KB):
 *
 *   {"v":1,"me":1,"t":[
 *     {"a":1,"r":1,"rt":0,"q":0,"d":"-MTWTF-","w":0,"hm":420,
 *      "tp":"home/lt/cmd","pl":"ON","l":"morning lights"},
 *     {"a":0}, ...
 *   ]}
 *
 *   v   schema version (1)
 *   me  master enable (0/1) — default 1
 *   t   array of TIMERS_SLOT_COUNT entries (missing trailing entries default
 *       to {arm:0})
 *   a   arm     r repeat   rt retain   q qos
 *   d   days mask string (7 chars, SMTWTFS — '-' or '0' = off)
 *   w   window (0..15)    hm minute_of_day (0..1439)
 *   tp  topic   pl payload   l label
 *
 * The compact keys keep 16 fully-populated slots well under the 4 KB NVS
 * value soft-limit. Long-form keys are accepted on parse (for hand-edits)
 * but never emitted.
 */

#include "timers.h"
#include "mqtt_broker.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "timers";

#define NVS_NS              "mqtt_cfg"
#define NVS_KEY             "timers"
#define NVS_BLOB_MAX        5120  /* hard cap on the JSON blob size */

/* Anything below this means the SNTP clock hasn't synced yet (any time
 * before 2023 is treated as "not real"). Matches the guard in §3 of the
 * plan and prevents bogus fires on cold boot. */
#define MIN_VALID_UNIX      1700000000

/* ===================================================================== */
/* Module state                                                          */
/* ===================================================================== */

static SemaphoreHandle_t s_lock = NULL;
static timer_slot_t      s_slots[TIMERS_SLOT_COUNT];
static bool              s_master_enabled = true;
static bool              s_initialized = false;

/* One per slot: the UTC minute (unix/60) at which we last fired this slot.
 * Used to dedup fall-back (DST end repeats an hour) and to avoid double-
 * firing within the same minute. Reset to 0 on slot edit. */
static int64_t           s_last_fire_utc_min[TIMERS_SLOT_COUNT];

/* Per-day jitter cache. Stable within a (slot, day-of-year) so the random
 * offset doesn't dance second-to-second. */
static int               s_jitter_yday[TIMERS_SLOT_COUNT];
static int               s_jitter_minutes[TIMERS_SLOT_COUNT];

static uint32_t          s_dropped = 0;
static bool              s_logged_no_broker = false;

/* ===================================================================== */
/* Locking helpers                                                       */
/* ===================================================================== */

#define LOCK()    do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK()  do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

/* ===================================================================== */
/* Day mask                                                              */
/* ===================================================================== */

bool timers_days_from_string(const char *s, uint8_t *out_mask)
{
    if (!s || !out_mask) return false;
    if (strlen(s) != 7) return false;
    uint8_t m = 0;
    for (int i = 0; i < 7; i++) {
        char c = s[i];
        if (c != '-' && c != '0') m |= (uint8_t)(1u << i);
    }
    *out_mask = m;
    return true;
}

void timers_days_to_string(uint8_t mask, char out[8])
{
    static const char letters[8] = "SMTWTFS";
    for (int i = 0; i < 7; i++) {
        out[i] = (mask & (1u << i)) ? letters[i] : '-';
    }
    out[7] = '\0';
}

/* ===================================================================== */
/* Validation                                                            */
/* ===================================================================== */

static bool topic_is_valid_publish(const char *t, size_t n)
{
    if (n == 0 || n > TIMERS_TOPIC_MAX) return false;
    if (t[0] == '$') return false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c == '#' || c == '+' || c == '\0') return false;
        if ((unsigned char)c < 0x20) return false;
    }
    return true;
}

esp_err_t timers_validate(const timer_slot_t *in, char *err_out, size_t err_out_size)
{
#define BAIL(msg) do { if (err_out && err_out_size) { strncpy(err_out, (msg), err_out_size - 1); err_out[err_out_size - 1] = '\0'; } return ESP_ERR_INVALID_ARG; } while (0)
    if (!in) BAIL("null input");
    if (in->qos > 1) BAIL("qos must be 0 or 1");
    if (in->window > 15) BAIL("window must be 0..15");
    if (in->minute_of_day > 1439) BAIL("time must be 00:00..23:59");
    if (in->days & ~TIMERS_DAYS_ALL) BAIL("days mask has invalid bits");

    /* Topic only required when armed. An unarmed slot may carry a draft. */
    if (in->arm) {
        size_t tn = strnlen(in->topic, TIMERS_TOPIC_MAX + 1);
        if (tn == 0) BAIL("topic required when armed");
        if (tn > TIMERS_TOPIC_MAX) BAIL("topic too long");
        if (!topic_is_valid_publish(in->topic, tn)) BAIL("invalid topic (wildcards, $SYS, or control chars not allowed)");
        if (in->days == 0) BAIL("at least one day must be selected");
    }
    if (in->payload_len > TIMERS_PAYLOAD_MAX) BAIL("payload too long");
    size_t ln = strnlen(in->label, TIMERS_LABEL_MAX + 1);
    if (ln > TIMERS_LABEL_MAX) BAIL("label too long");
    return ESP_OK;
#undef BAIL
}

/* ===================================================================== */
/* Minimal JSON parser/serializer (tailored for this blob only)           */
/* ===================================================================== */

/* Skip whitespace. Returns pointer to first non-ws char. */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a JSON string starting at *pp (which must point at the opening
 * quote). Decodes \\ \" \n \r \t \/ and \uXXXX (BMP only). Writes up to
 * out_size-1 bytes (always NUL-terminated). Returns parsed byte length
 * (excluding NUL) on success, or -1 on error. On success advances *pp
 * past the closing quote. */
static int parse_json_string(const char **pp, char *out, size_t out_size,
                             size_t *out_bytes)
{
    const char *p = *pp;
    if (*p != '"') return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\') {
            p++;
            char e = *p;
            if (!e) return -1;
            char dec;
            switch (e) {
                case '"': dec = '"'; break;
                case '\\': dec = '\\'; break;
                case '/': dec = '/'; break;
                case 'n': dec = '\n'; break;
                case 'r': dec = '\r'; break;
                case 't': dec = '\t'; break;
                case 'b': dec = '\b'; break;
                case 'f': dec = '\f'; break;
                case 'u': {
                    /* BMP-only; encode to UTF-8. */
                    if (!p[1] || !p[2] || !p[3] || !p[4]) return -1;
                    unsigned cp = 0;
                    for (int i = 1; i <= 4; i++) {
                        char h = p[i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return -1;
                    }
                    p += 5;
                    /* Encode UTF-8 */
                    if (cp < 0x80) {
                        if (o + 1 >= out_size) return -1;
                        out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (o + 2 >= out_size) return -1;
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (o + 3 >= out_size) return -1;
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    continue;
                }
                default: return -1;
            }
            if (o + 1 >= out_size) return -1;
            out[o++] = dec;
            p++;
        } else {
            if (o + 1 >= out_size) return -1;
            out[o++] = c;
            p++;
        }
    }
    if (*p != '"') return -1;
    p++;
    out[o] = '\0';
    if (out_bytes) *out_bytes = o;
    *pp = p;
    return (int)o;
}

/* Parse a JSON integer at *pp into *out. Advances *pp. Returns 0 on
 * success, -1 on error. */
static int parse_json_int(const char **pp, long *out)
{
    const char *p = *pp;
    char *end;
    errno = 0;
    long v = strtol(p, &end, 10);
    if (end == p || errno) return -1;
    *out = v;
    *pp = end;
    return 0;
}

/* Skip a JSON value of any type (for unknown keys / forward-compat). */
static int skip_json_value(const char **pp)
{
    const char *p = skip_ws(*pp);
    if (*p == '"') {
        /* string */
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p != '"') return -1;
        p++;
    } else if (*p == '{' || *p == '[') {
        char close = (*p == '{') ? '}' : ']';
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                if (*p == '"') p++;
            } else if (*p == '{' || *p == '[') { depth++; p++; }
            else if (*p == '}' || *p == ']') { if (*p == close && depth == 1) depth = 0; depth--; p++; }
            else p++;
        }
    } else {
        /* number / true / false / null */
        while (*p && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    }
    *pp = p;
    return 0;
}

/* Parse a single slot object. *pp must point at '{'. Advances past '}'. */
static int parse_slot_object(const char **pp, timer_slot_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = skip_ws(*pp);
    if (*p != '{') return -1;
    p++;
    p = skip_ws(p);
    if (*p == '}') { p++; *pp = p; return 0; }  /* empty {} */

    while (*p) {
        p = skip_ws(p);
        char key[16];
        size_t klen = 0;
        if (parse_json_string(&p, key, sizeof(key), &klen) < 0) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p++;
        p = skip_ws(p);

        long iv = 0;
        if (strcmp(key, "a") == 0 || strcmp(key, "arm") == 0) {
            if (parse_json_int(&p, &iv) < 0) return -1;
            out->arm = (iv != 0);
        } else if (strcmp(key, "r") == 0 || strcmp(key, "repeat") == 0) {
            if (parse_json_int(&p, &iv) < 0) return -1;
            out->repeat = (iv != 0);
        } else if (strcmp(key, "rt") == 0 || strcmp(key, "retain") == 0) {
            if (parse_json_int(&p, &iv) < 0) return -1;
            out->retain = (iv != 0);
        } else if (strcmp(key, "q") == 0 || strcmp(key, "qos") == 0) {
            if (parse_json_int(&p, &iv) < 0) return -1;
            if (iv < 0 || iv > 1) return -1;
            out->qos = (uint8_t)iv;
        } else if (strcmp(key, "w") == 0 || strcmp(key, "window") == 0) {
            if (parse_json_int(&p, &iv) < 0) return -1;
            if (iv < 0 || iv > 15) return -1;
            out->window = (uint8_t)iv;
        } else if (strcmp(key, "hm") == 0 || strcmp(key, "min") == 0) {
            if (parse_json_int(&p, &iv) < 0) return -1;
            if (iv < 0 || iv > 1439) return -1;
            out->minute_of_day = (uint16_t)iv;
        } else if (strcmp(key, "d") == 0 || strcmp(key, "days") == 0) {
            char dbuf[9] = "";
            size_t dl = 0;
            if (parse_json_string(&p, dbuf, sizeof(dbuf), &dl) < 0) return -1;
            uint8_t m;
            if (!timers_days_from_string(dbuf, &m)) return -1;
            out->days = m;
        } else if (strcmp(key, "tp") == 0 || strcmp(key, "topic") == 0) {
            size_t tl = 0;
            if (parse_json_string(&p, out->topic, sizeof(out->topic), &tl) < 0) return -1;
        } else if (strcmp(key, "pl") == 0 || strcmp(key, "payload") == 0) {
            size_t pl = 0;
            if (parse_json_string(&p, out->payload, sizeof(out->payload), &pl) < 0) return -1;
            out->payload_len = (uint16_t)pl;
        } else if (strcmp(key, "l") == 0 || strcmp(key, "label") == 0) {
            size_t ll = 0;
            if (parse_json_string(&p, out->label, sizeof(out->label), &ll) < 0) return -1;
        } else {
            /* Unknown key — skip value for forward compatibility. */
            if (skip_json_value(&p) < 0) return -1;
        }
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return -1;
    }
    *pp = p;
    return 0;
}

/* Parse the whole blob. Unknown / extra slots beyond TIMERS_SLOT_COUNT are
 * dropped. Missing slots default to {arm:0}. */
static int parse_blob(const char *blob, bool *master_out)
{
    const char *p = skip_ws(blob);
    if (*p != '{') return -1;
    p++;
    bool master = true;
    int slot_idx = 0;
    /* Reset slots up front */
    memset(s_slots, 0, sizeof(s_slots));

    p = skip_ws(p);
    if (*p == '}') { *master_out = master; return 0; }

    while (*p) {
        p = skip_ws(p);
        char key[16];
        size_t klen = 0;
        if (parse_json_string(&p, key, sizeof(key), &klen) < 0) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p++;
        p = skip_ws(p);

        if (strcmp(key, "v") == 0) {
            long iv;
            if (parse_json_int(&p, &iv) < 0) return -1;
            /* No-op: future migrations key off this. */
        } else if (strcmp(key, "me") == 0 || strcmp(key, "master") == 0) {
            long iv;
            if (parse_json_int(&p, &iv) < 0) return -1;
            master = (iv != 0);
        } else if (strcmp(key, "t") == 0 || strcmp(key, "timers") == 0) {
            if (*p != '[') return -1;
            p++;
            slot_idx = 0;
            p = skip_ws(p);
            if (*p == ']') { p++; }
            else {
                while (*p) {
                    timer_slot_t tmp;
                    if (parse_slot_object(&p, &tmp) < 0) return -1;
                    if (slot_idx < TIMERS_SLOT_COUNT) {
                        s_slots[slot_idx] = tmp;
                    }
                    slot_idx++;
                    p = skip_ws(p);
                    if (*p == ',') { p++; p = skip_ws(p); continue; }
                    if (*p == ']') { p++; break; }
                    return -1;
                }
            }
        } else {
            if (skip_json_value(&p) < 0) return -1;
        }
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return -1;
    }
    *master_out = master;
    return 0;
}

/* ===================================================================== */
/* Serialization                                                         */
/* ===================================================================== */

/* Append a JSON-escaped string. Returns 0 / -1. */
static int append_json_string(char *buf, size_t bufsz, int *pos,
                              const char *s, size_t n)
{
    int p = *pos;
    if (p + 1 >= (int)bufsz) return -1;
    buf[p++] = '"';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char tmp[8];
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            default:
                if (c < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    esc = tmp;
                }
                break;
        }
        if (esc) {
            size_t el = strlen(esc);
            if (p + (int)el >= (int)bufsz) return -1;
            memcpy(buf + p, esc, el);
            p += (int)el;
        } else {
            if (p + 1 >= (int)bufsz) return -1;
            buf[p++] = (char)c;
        }
    }
    if (p + 1 >= (int)bufsz) return -1;
    buf[p++] = '"';
    *pos = p;
    return 0;
}

/* Serialize current s_slots + s_master_enabled into a compact JSON blob.
 * Returns bytes written (excluding NUL), or -1 on overflow. */
static int serialize_blob(char *buf, size_t bufsz)
{
    int p = 0;
    int n = snprintf(buf + p, bufsz - p, "{\"v\":1,\"me\":%d,\"t\":[",
                     s_master_enabled ? 1 : 0);
    if (n < 0 || (size_t)(p + n) >= bufsz) return -1;
    p += n;

    for (int i = 0; i < TIMERS_SLOT_COUNT; i++) {
        if (i > 0) {
            if (p + 1 >= (int)bufsz) return -1;
            buf[p++] = ',';
        }
        const timer_slot_t *s = &s_slots[i];
        /* Empty slots collapse to {} or {"a":0} to save bytes. */
        if (!s->arm && s->topic[0] == '\0' && s->label[0] == '\0' &&
            s->payload_len == 0 && s->days == 0 && s->minute_of_day == 0 &&
            s->window == 0 && !s->repeat && !s->retain && s->qos == 0) {
            n = snprintf(buf + p, bufsz - p, "{}");
            if (n < 0 || (size_t)(p + n) >= bufsz) return -1;
            p += n;
            continue;
        }
        char days_str[8];
        timers_days_to_string(s->days, days_str);
        n = snprintf(buf + p, bufsz - p,
                     "{\"a\":%d,\"r\":%d,\"rt\":%d,\"q\":%u,\"w\":%u,\"hm\":%u,\"d\":\"%s\"",
                     s->arm ? 1 : 0,
                     s->repeat ? 1 : 0,
                     s->retain ? 1 : 0,
                     (unsigned)s->qos,
                     (unsigned)s->window,
                     (unsigned)s->minute_of_day,
                     days_str);
        if (n < 0 || (size_t)(p + n) >= bufsz) return -1;
        p += n;

        if (s->topic[0]) {
            n = snprintf(buf + p, bufsz - p, ",\"tp\":");
            if (n < 0 || (size_t)(p + n) >= bufsz) return -1;
            p += n;
            if (append_json_string(buf, bufsz, &p, s->topic,
                                   strnlen(s->topic, TIMERS_TOPIC_MAX)) < 0) return -1;
        }
        if (s->payload_len > 0) {
            n = snprintf(buf + p, bufsz - p, ",\"pl\":");
            if (n < 0 || (size_t)(p + n) >= bufsz) return -1;
            p += n;
            if (append_json_string(buf, bufsz, &p, s->payload, s->payload_len) < 0) return -1;
        }
        if (s->label[0]) {
            n = snprintf(buf + p, bufsz - p, ",\"l\":");
            if (n < 0 || (size_t)(p + n) >= bufsz) return -1;
            p += n;
            if (append_json_string(buf, bufsz, &p, s->label,
                                   strnlen(s->label, TIMERS_LABEL_MAX)) < 0) return -1;
        }
        if (p + 1 >= (int)bufsz) return -1;
        buf[p++] = '}';
    }
    if (p + 3 >= (int)bufsz) return -1;
    buf[p++] = ']';
    buf[p++] = '}';
    buf[p] = '\0';
    return p;
}

/* ===================================================================== */
/* Persistence                                                           */
/* ===================================================================== */

static esp_err_t load_from_nvs(void)
{
    /* Default: master enabled, all slots empty. */
    memset(s_slots, 0, sizeof(s_slots));
    s_master_enabled = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;  /* fine — first boot */
    }
    size_t needed = 0;
    esp_err_t err = nvs_get_str(h, NVS_KEY, NULL, &needed);
    if (err != ESP_OK || needed == 0) {
        nvs_close(h);
        return ESP_OK;
    }
    if (needed > NVS_BLOB_MAX) {
        ESP_LOGW(TAG, "stored blob too large (%u bytes) — ignoring", (unsigned)needed);
        nvs_close(h);
        return ESP_OK;
    }
    char *buf = (char *)malloc(needed);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }
    err = nvs_get_str(h, NVS_KEY, buf, &needed);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    bool master = true;
    int rc = parse_blob(buf, &master);
    free(buf);
    if (rc < 0) {
        ESP_LOGW(TAG, "blob parse failed — defaults applied");
        memset(s_slots, 0, sizeof(s_slots));
        s_master_enabled = true;
        return ESP_OK;
    }
    s_master_enabled = master;
    int armed = 0;
    for (int i = 0; i < TIMERS_SLOT_COUNT; i++) if (s_slots[i].arm) armed++;
    ESP_LOGI(TAG, "loaded %d/%d armed slot(s), master=%s",
             armed, TIMERS_SLOT_COUNT, s_master_enabled ? "on" : "off");
    return ESP_OK;
}

static esp_err_t save_to_nvs_locked(void)
{
    char *buf = (char *)malloc(NVS_BLOB_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    int n = serialize_blob(buf, NVS_BLOB_MAX);
    if (n < 0) {
        ESP_LOGE(TAG, "serialize overflow — refusing to save");
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { free(buf); return err; }
    err = nvs_set_str(h, NVS_KEY, buf);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    free(buf);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved (%d bytes)", n);
    }
    return err;
}

/* ===================================================================== */
/* Next-fire computation                                                 */
/* ===================================================================== */

/* Compute UTC unix time of the next fire for slot index (0-based).
 * Walks forward at most 8 days; returns 0 on no-match / disarmed. */
static int64_t next_fire_for_locked(int idx)
{
    if (idx < 0 || idx >= TIMERS_SLOT_COUNT) return 0;
    const timer_slot_t *s = &s_slots[idx];
    if (!s->arm || !s_master_enabled || s->days == 0) return 0;

    time_t now = time(NULL);
    if (now < MIN_VALID_UNIX) return 0;

    struct tm lt;
    localtime_r(&now, &lt);

    for (int day_off = 0; day_off < 8; day_off++) {
        struct tm cand = lt;
        cand.tm_mday += day_off;
        cand.tm_hour = s->minute_of_day / 60;
        cand.tm_min  = s->minute_of_day % 60;
        cand.tm_sec  = 0;
        cand.tm_isdst = -1;  /* let mktime resolve DST */
        time_t cand_unix = mktime(&cand);
        if (cand_unix == (time_t)-1) {
            /* Spring-forward gap — this local time doesn't exist today. */
            continue;
        }
        if (cand_unix <= now) continue;
        /* Re-derive wday from the resolved struct tm (mktime normalizes). */
        int wday = cand.tm_wday;  /* 0=Sun .. 6=Sat */
        if (s->days & (1u << wday)) {
            return (int64_t)cand_unix;
        }
    }
    return 0;
}

int64_t timers_next_fire_unix(int slot_1based)
{
    if (slot_1based < 1 || slot_1based > TIMERS_SLOT_COUNT) return 0;
    int64_t out;
    LOCK();
    out = next_fire_for_locked(slot_1based - 1);
    UNLOCK();
    return out;
}

/* ===================================================================== */
/* Fire path                                                             */
/* ===================================================================== */

static void fire_slot_locked(int idx)
{
    const timer_slot_t *s = &s_slots[idx];
    size_t tn = strnlen(s->topic, TIMERS_TOPIC_MAX);
    bool ok = broker_publish_local(s->topic, tn,
                                   (const uint8_t *)s->payload, s->payload_len,
                                   s->qos, s->retain);
    if (ok) {
        ESP_LOGI(TAG, "fired slot %d -> %s (%u bytes, qos%u%s)",
                 idx + 1, s->topic, (unsigned)s->payload_len,
                 (unsigned)s->qos, s->retain ? ", retain" : "");
    } else {
        s_dropped++;
        if (!s_logged_no_broker) {
            ESP_LOGW(TAG, "publish queue full / broker unavailable — slot %d dropped", idx + 1);
            s_logged_no_broker = true;
        }
    }
}

esp_err_t timers_fire_now(int slot_1based)
{
    if (slot_1based < 1 || slot_1based > TIMERS_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    int idx = slot_1based - 1;
    LOCK();
    if (!s_slots[idx].arm || s_slots[idx].topic[0] == '\0') {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    fire_slot_locked(idx);
    UNLOCK();
    return ESP_OK;
}

uint32_t timers_dropped_count(void) { return s_dropped; }

/* ===================================================================== */
/* Public getters / setters                                              */
/* ===================================================================== */

bool timers_master_enabled(void)
{
    bool v;
    LOCK();
    v = s_master_enabled;
    UNLOCK();
    return v;
}

void timers_set_master_enabled(bool enabled)
{
    LOCK();
    if (s_master_enabled != enabled) {
        s_master_enabled = enabled;
        save_to_nvs_locked();
    }
    UNLOCK();
}

bool timers_get(int slot_1based, timer_slot_t *out)
{
    if (slot_1based < 1 || slot_1based > TIMERS_SLOT_COUNT || !out) return false;
    LOCK();
    *out = s_slots[slot_1based - 1];
    UNLOCK();
    return true;
}

esp_err_t timers_set(int slot_1based, const timer_slot_t *in,
                     char *err_out, size_t err_out_size)
{
    if (slot_1based < 1 || slot_1based > TIMERS_SLOT_COUNT) {
        if (err_out && err_out_size) snprintf(err_out, err_out_size, "slot out of range");
        return ESP_ERR_INVALID_ARG;
    }
    if (!in) return ESP_ERR_INVALID_ARG;
    esp_err_t v = timers_validate(in, err_out, err_out_size);
    if (v != ESP_OK) return v;

    int idx = slot_1based - 1;
    LOCK();
    s_slots[idx] = *in;
    /* Force terminators. */
    s_slots[idx].topic[TIMERS_TOPIC_MAX] = '\0';
    s_slots[idx].label[TIMERS_LABEL_MAX] = '\0';
    if (s_slots[idx].payload_len > TIMERS_PAYLOAD_MAX) s_slots[idx].payload_len = TIMERS_PAYLOAD_MAX;
    /* Clear last-fire cache so a backward time change can't replay. */
    s_last_fire_utc_min[idx] = 0;
    s_jitter_yday[idx] = -1;
    esp_err_t err = save_to_nvs_locked();
    UNLOCK();
    return err;
}

esp_err_t timers_clear(int slot_1based)
{
    if (slot_1based < 1 || slot_1based > TIMERS_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    int idx = slot_1based - 1;
    LOCK();
    memset(&s_slots[idx], 0, sizeof(s_slots[idx]));
    s_last_fire_utc_min[idx] = 0;
    s_jitter_yday[idx] = -1;
    esp_err_t err = save_to_nvs_locked();
    UNLOCK();
    return err;
}

/* ===================================================================== */
/* Scheduler task                                                        */
/* ===================================================================== */

/* xorshift32, seeded per (slot, day-of-year). Cheap, stable, no rand() lock. */
static uint32_t hash32(uint32_t x)
{
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x ? x : 1;
}

static void sweep(time_t now_utc, const struct tm *lt)
{
    int64_t now_utc_min = (int64_t)now_utc / 60;
    for (int i = 0; i < TIMERS_SLOT_COUNT; i++) {
        timer_slot_t *s = &s_slots[i];
        if (!s->arm) continue;
        if (s->days == 0) continue;
        if (!(s->days & (1u << lt->tm_wday))) continue;

        /* Refresh jitter cache once per local day. */
        if (s_jitter_yday[i] != lt->tm_yday) {
            s_jitter_yday[i] = lt->tm_yday;
            if (s->window > 0) {
                uint32_t r = hash32((uint32_t)((i * 131u) ^ (uint32_t)lt->tm_yday ^ 0xA5A5u));
                int span = (int)s->window * 2 + 1;  /* [-window, +window] */
                int j = (int)(r % (uint32_t)span) - (int)s->window;
                s_jitter_minutes[i] = j;
            } else {
                s_jitter_minutes[i] = 0;
            }
        }

        int target_minute = (int)s->minute_of_day + s_jitter_minutes[i];
        /* Today only — don't fire jitter that pushes past midnight, to
         * keep one fire per local day. */
        if (target_minute < 0 || target_minute > 1439) continue;
        int now_local_minute = lt->tm_hour * 60 + lt->tm_min;
        if (now_local_minute != target_minute) continue;
        /* Dedup: at most one fire per (slot, UTC minute). Survives DST
         * fall-back because UTC keeps moving forward. */
        if (s_last_fire_utc_min[i] == now_utc_min) continue;

        s_last_fire_utc_min[i] = now_utc_min;
        fire_slot_locked(i);

        if (!s->repeat) {
            /* Once-mode: disarm and persist. save is light (single NVS
             * write) but synchronous; fine at 1 Hz worst case. */
            s->arm = false;
            save_to_nvs_locked();
        }
    }
}

static void timers_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    time_t last_now = 0;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));

        time_t now = time(NULL);
        if (now < MIN_VALID_UNIX) continue;  /* clock not synced yet */

        /* Clock jump guard: if we just woke up after a >60s gap (boot or
         * SNTP step), don't retro-fire missed slots. */
        time_t delta = (last_now != 0) ? (now - last_now) : 0;
        bool large_jump = (last_now != 0) && (delta > 60 || delta < -60);
        last_now = now;
        if (large_jump) {
            ESP_LOGW(TAG, "clock jumped (%lds) — suppressing catch-up sweep",
                     (long)delta);
            continue;
        }

        if (!s_master_enabled) continue;

        struct tm lt;
        localtime_r(&now, &lt);

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            sweep(now, &lt);
            xSemaphoreGive(s_lock);
        }
    }
}

esp_err_t timers_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    for (int i = 0; i < TIMERS_SLOT_COUNT; i++) {
        s_last_fire_utc_min[i] = 0;
        s_jitter_yday[i] = -1;
        s_jitter_minutes[i] = 0;
    }
    load_from_nvs();
    BaseType_t r = xTaskCreate(timers_task, "timers", 3072, NULL, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "scheduler started (1Hz, %d slots)", TIMERS_SLOT_COUNT);
    return ESP_OK;
}
