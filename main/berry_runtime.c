/* Berry runtime — Phase 1 + 2 + 3 + multi-slot scripts.
 *
 * P1: Berry VM, REPL via berry_eval().
 * P2: NVS persistence, autoexec, berry_restart().
 * P3: topic-event hook, berry_publish_topic_event().
 * Multi-slot: 4 named script slots (berry_s0_nm/sc/en … berry_s3_nm/sc/en).
 *   Legacy single-slot keys (berry_en, berry_script) are migrated to slot 0
 *   on first access when the new keys are absent.
 */
#include "berry_runtime.h"
#include "berry_mod_mqtt.h"
#include "berry_mod_http.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "berry.h"

static const char *TAG = "berry";

#define BERRY_NVS_NS     "mqtt_cfg"  /* shared with portal/timers; see AGENTS.md §4 */

/* Legacy single-slot keys (read-only; migrated to slot 0 on first access). */
#define BERRY_NVS_LEGACY_EN    "berry_en"
#define BERRY_NVS_LEGACY_SCR   "berry_script"

/* Multi-slot key name buffers.  berry_sN_{nm,sc,en}  (11 chars max → ≤15) */
static void slot_key(char *out, int slot, const char *suffix)
{
    /* e.g. "berry_s0_nm", "berry_s3_sc", "berry_s2_en" */
    snprintf(out, 16, "berry_s%d_%s", slot, suffix);
}

/* ---- NVS slot helpers ---- */

static bool nvs_slot_get_enabled(int slot)
{
    char key[16]; slot_key(key, slot, "en");
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(BERRY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void nvs_slot_set_enabled(int slot, bool en)
{
    char key[16]; slot_key(key, slot, "en");
    nvs_handle_t h;
    if (nvs_open(BERRY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, en ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static size_t nvs_slot_get_script(int slot, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return 0;
    buf[0] = '\0';
    char key[16]; slot_key(key, slot, "sc");
    nvs_handle_t h;
    if (nvs_open(BERRY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = bufsz;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) { buf[0] = '\0'; return 0; }
    return len > 0 ? len - 1 : 0;
}

static bool nvs_slot_set_script(int slot, const char *src, size_t src_len)
{
    if (!src) src = "";
    if (src_len > BERRY_SCRIPT_MAX) src_len = BERRY_SCRIPT_MAX;
    char *tmp = (char *)malloc(src_len + 1);
    if (!tmp) return false;
    memcpy(tmp, src, src_len);
    tmp[src_len] = '\0';
    char key[16]; slot_key(key, slot, "sc");
    nvs_handle_t h;
    esp_err_t err = nvs_open(BERRY_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { free(tmp); return false; }
    err = nvs_set_str(h, key, tmp);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    free(tmp);
    return err == ESP_OK;
}

static size_t nvs_slot_get_label(int slot, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return 0;
    buf[0] = '\0';
    char key[16]; slot_key(key, slot, "nm");
    nvs_handle_t h;
    if (nvs_open(BERRY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = bufsz;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) { buf[0] = '\0'; return 0; }
    return len > 0 ? len - 1 : 0;
}

static bool nvs_slot_set_label(int slot, const char *label, size_t label_len)
{
    if (!label) label = "";
    if (label_len >= BERRY_LABEL_MAX) label_len = BERRY_LABEL_MAX - 1;
    char tmp[BERRY_LABEL_MAX];
    memcpy(tmp, label, label_len);
    tmp[label_len] = '\0';
    char key[16]; slot_key(key, slot, "nm");
    nvs_handle_t h;
    esp_err_t err = nvs_open(BERRY_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    err = nvs_set_str(h, key, tmp);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

/* One-time migration: if slot-0 keys are absent but legacy keys exist,
 * copy legacy data into slot 0 and erase legacy keys.
 * Called once from berry_init() before berry_task starts. */
static void migrate_legacy_slot(void)
{
    /* Check if slot-0 script key already exists. */
    char sc_key[16]; slot_key(sc_key, 0, "sc");
    nvs_handle_t h;
    if (nvs_open(BERRY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t probe = 0;
    bool slot0_exists = (nvs_get_str(h, sc_key, NULL, &probe) != ESP_ERR_NVS_NOT_FOUND);
    nvs_close(h);
    if (slot0_exists) return; /* already migrated */

    /* Read legacy keys. */
    uint8_t legacy_en = 0;
    char *legacy_scr = (char *)malloc(3501);
    if (!legacy_scr) return;
    legacy_scr[0] = '\0';
    size_t legacy_len = 0;

    if (nvs_open(BERRY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, BERRY_NVS_LEGACY_EN, &legacy_en);
        size_t sz = 3501;
        if (nvs_get_str(h, BERRY_NVS_LEGACY_SCR, legacy_scr, &sz) == ESP_OK && sz > 1)
            legacy_len = sz - 1;
        nvs_close(h);
    }

    /* Write to slot 0 (only if there was something to migrate). */
    if (legacy_len > 0 || legacy_en) {
        nvs_slot_set_script(0, legacy_scr, legacy_len);
        nvs_slot_set_enabled(0, legacy_en != 0);
        nvs_slot_set_label(0, "autoexec", 8);
        ESP_LOGI(TAG, "berry: migrated legacy script (%u bytes, en=%d) → slot 0",
                 (unsigned)legacy_len, legacy_en);
        /* Erase legacy keys so migration doesn't re-run. */
        if (nvs_open(BERRY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, BERRY_NVS_LEGACY_EN);
            nvs_erase_key(h, BERRY_NVS_LEGACY_SCR);
            nvs_commit(h);
            nvs_close(h);
        }
    } else {
        /* No legacy data — write empty slot 0 script key so migration
         * doesn't run again. */
        nvs_slot_set_script(0, "", 0);
    }
    free(legacy_scr);
}
#define BERRY_LOG_RING_SZ   4096
static char            s_log_ring[BERRY_LOG_RING_SZ];
static size_t          s_log_head;
static size_t          s_log_total;
static SemaphoreHandle_t s_log_mux;

static void log_push(const char *buf, size_t len)
{
    if (!s_log_mux || len == 0) return;
    if (xSemaphoreTake(s_log_mux, pdMS_TO_TICKS(20)) != pdTRUE) return;
    for (size_t i = 0; i < len; i++) {
        s_log_ring[s_log_head] = buf[i];
        s_log_head = (s_log_head + 1) & (BERRY_LOG_RING_SZ - 1);
        s_log_total++;
    }
    xSemaphoreGive(s_log_mux);
}

/* Called by port/be_port.c::be_writebuffer */
void berry_port_stdout_write(const char *buffer, size_t length)
{
    log_push(buffer, length);
    char line[160];
    size_t n = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
    memcpy(line, buffer, n);
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
    line[n] = '\0';
    if (n > 0) ESP_LOGI(TAG, "%s", line);
}

/* Format a [ts ms] prefix + line into the log ring. Used by runtime
 * events (autoexec start, errors, restart) so the log pane shows them
 * alongside script print() output. */
static void log_event(const char *fmt, ...)
{
    char line[160];
    int64_t now = esp_timer_get_time() / 1000;  /* ms */
    int n = snprintf(line, sizeof(line), "[%lld] ", (long long)now);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    if (n > 0 && line[n-1] != '\n' && (size_t)n < sizeof(line) - 1) {
        line[n++] = '\n';
        line[n] = '\0';
    }
    log_push(line, (size_t)n);
    /* Mirror to ESP_LOG without the trailing newline. */
    if (n > 0 && line[n-1] == '\n') line[n-1] = '\0';
    ESP_LOGI(TAG, "%s", line);
}

size_t berry_log_snapshot(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0 || !s_log_mux) return 0;
    if (xSemaphoreTake(s_log_mux, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    size_t out = 0;
    if (s_log_total < BERRY_LOG_RING_SZ) {
        size_t n = s_log_head < bufsz - 1 ? s_log_head : bufsz - 1;
        memcpy(buf, s_log_ring, n);
        out = n;
    } else {
        size_t avail = BERRY_LOG_RING_SZ;
        size_t n = avail < bufsz - 1 ? avail : bufsz - 1;
        size_t tail_len = BERRY_LOG_RING_SZ - s_log_head;
        if (tail_len > n) tail_len = n;
        memcpy(buf, s_log_ring + s_log_head, tail_len);
        size_t rem = n - tail_len;
        if (rem > 0) memcpy(buf + tail_len, s_log_ring, rem);
        out = n;
    }
    buf[out] = '\0';
    xSemaphoreGive(s_log_mux);
    return out;
}

/* --- Eval / control / topic-event queue ---
 * One queue carrying tagged commands.
 *   EVAL          - runs a snippet, signals caller via done semaphore.
 *   RESTART       - tears down the VM and rebuilds it.
 *   LOAD_AUTOEXEC - re-runs the persisted script.
 *   TOPIC         - delivers a broker fanout event to mqtt.subscribe()
 *                   callbacks. Posted from broker_task with no caller
 *                   waiting on it; berry_task frees the heap fields.
 */
typedef enum {
    BCMD_EVAL,
    BCMD_RESTART,
    BCMD_LOAD_AUTOEXEC,
    BCMD_TOPIC,
} bcmd_kind_t;

typedef struct {
    bcmd_kind_t       kind;
    /* EVAL / RESTART / LOAD_AUTOEXEC: */
    const char       *src;
    char             *result;
    size_t            result_sz;
    bool              ok;
    SemaphoreHandle_t done;
    /* TOPIC: heap-allocated, freed by berry_task after dispatch. */
    char             *topic;
    size_t            topic_len;
    uint8_t          *payload;
    size_t            payload_len;
} bcmd_t;

static QueueHandle_t s_cmd_q;

/* --- Topic subscription accounting ---
 * berry_mod_mqtt.c keeps the authoritative list of {filter, callable}
 * pairs inside Berry (global `_be_mqtt_subs`). It calls
 * berry_set_topic_subs_count() whenever the list changes; we cache
 * the count here so broker_task can skip the topic-event allocation
 * when no scripts are listening. */
static volatile int s_topic_subs_count;
static unsigned     s_topic_events_total;
static unsigned     s_topic_events_dropped;

bool berry_has_topic_subs(void) { return s_topic_subs_count > 0; }
void berry_set_topic_subs_count(int n) { s_topic_subs_count = n; }

/* --- VM state --- */
static bvm                  *s_vm;
static unsigned              s_evals_total;
static unsigned              s_errors_total;
static volatile bool         s_running;
static int64_t               s_vm_start_us;

static void format_value(bvm *vm, int idx, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (be_isstring(vm, idx)) {
        const char *s = be_tostring(vm, idx);
        snprintf(out, out_sz, "%s", s ? s : "");
    } else if (be_isint(vm, idx)) {
        snprintf(out, out_sz, "%lld", (long long)be_toint(vm, idx));
    } else if (be_isreal(vm, idx)) {
        snprintf(out, out_sz, "%g", (double)be_toreal(vm, idx));
    } else if (be_isbool(vm, idx)) {
        snprintf(out, out_sz, "%s", be_tobool(vm, idx) ? "true" : "false");
    } else if (be_isnil(vm, idx)) {
        snprintf(out, out_sz, "nil");
    } else {
        const char *t = be_typename(vm, idx);
        snprintf(out, out_sz, "<%s>", t ? t : "value");
    }
}

/* Execute `src` on the current VM. result/result_sz may be NULL/0 if
 * the caller doesn't care about the return value (autoexec case);
 * errors still go to the log ring. */
static bool run_source(const char *src, char *result, size_t result_sz)
{
    s_evals_total++;
    if (!s_vm) {
        if (result) snprintf(result, result_sz, "vm not running");
        return false;
    }
    int rc = be_loadstring(s_vm, src);
    if (rc != 0) {
        s_errors_total++;
        const char *msg = be_tostring(s_vm, -1);
        if (result) snprintf(result, result_sz, "compile error: %s", msg ? msg : "?");
        log_event("compile error: %s", msg ? msg : "?");
        be_pop(s_vm, 1);
        return false;
    }
    rc = be_pcall(s_vm, 0);
    if (rc != 0) {
        s_errors_total++;
        const char *msg = be_tostring(s_vm, -1);
        if (result) snprintf(result, result_sz, "runtime error: %s", msg ? msg : "?");
        log_event("runtime error: %s", msg ? msg : "?");
        be_pop(s_vm, be_top(s_vm));
        return false;
    }
    if (result && result_sz > 0) {
        format_value(s_vm, -1, result, result_sz);
    }
    be_pop(s_vm, be_top(s_vm));
    return true;
}

static void vm_destroy(void)
{
    if (s_vm) {
        be_vm_delete(s_vm);
        s_vm = NULL;
    }
    s_running = false;
}

/* MQTT topic-matching glue. We re-use the broker's wildcard helper
 * (mqtt_topic_matches in mqtt_parser.c) rather than re-implementing
 * `+` / `#` semantics. Forward-declared here to avoid pulling the
 * broker's full header into berry_runtime.c. */
extern bool mqtt_topic_matches(const char *filter, uint16_t filter_len,
                               const char *topic, uint16_t topic_len);

static void install_subs_list(void)
{
    /* Equivalent to `_be_mqtt_subs = []` at boot, so the native module
     * never has to special-case the first subscribe. Always pop the
     * list off the stack afterwards. */
    if (!s_vm) return;
    be_newlist(s_vm);
    be_setglobal(s_vm, "_be_mqtt_subs");
    be_pop(s_vm, 1);
}

static void vm_construct(void)
{
    vm_destroy();
    s_vm = be_vm_new();
    if (!s_vm) {
        log_event("be_vm_new() failed");
        return;
    }
    s_vm_start_us = esp_timer_get_time();
    s_running = true;
    s_topic_subs_count = 0;
    install_subs_list();
    berry_mod_mqtt_register(s_vm);  /* P3: register `mqtt` global module */
    berry_mod_http_register(s_vm);  /* P4: register `http` global module */
    log_event("VM up, free heap=%u",
              (unsigned)esp_get_free_heap_size());
}

/* Dispatch a single topic event: iterate _be_mqtt_subs, call fn(topic,
 * payload) for each matching filter. Errors are logged but never raised
 * out — one bad subscriber must not poison the others.
 *
 * Stack discipline:
 *   be_getindex pushes the result WITHOUT popping the key. We use be_remove
 *   to drop the key immediately after each getindex so the stack stays clean.
 */
static void dispatch_topic(const char *topic, size_t topic_len,
                           const char *payload, size_t payload_len)
{
    if (!s_vm) return;
    int top = be_top(s_vm);
    if (!be_getglobal(s_vm, "_be_mqtt_subs") || !be_islist(s_vm, -1)) {
        be_pop(s_vm, be_top(s_vm) - top);
        return;
    }
    /* Stack: [..., list] */
    int n = be_data_size(s_vm, -1);
    for (int i = 0; i < n; i++) {
        /* Get pair = subs[i]. Stack: [..., list, i, pair] then remove i */
        be_pushint(s_vm, i);
        if (!be_getindex(s_vm, -2)) {
            be_pop(s_vm, 2); /* nil result + key */
            continue;
        }
        be_remove(s_vm, -2); /* remove the int key; stack: [..., list, pair] */

        /* Get filter = pair[0]. Stack: [..., list, pair, 0, filter] then remove 0 */
        be_pushint(s_vm, 0);
        be_getindex(s_vm, -2);
        be_remove(s_vm, -2); /* remove key; stack: [..., list, pair, filter] */

        if (!be_isstring(s_vm, -1)) {
            be_pop(s_vm, 2); /* filter + pair */
            continue;
        }
        const char *filter = be_tostring(s_vm, -1);
        bool match = mqtt_topic_matches(filter, (uint16_t)strlen(filter),
                                        topic, (uint16_t)topic_len);
        be_pop(s_vm, 1); /* filter; stack: [..., list, pair] */

        if (!match) {
            be_pop(s_vm, 1); /* pair */
            continue;
        }

        /* Get fn = pair[1]. Stack: [..., list, pair, 1, fn] then remove 1 */
        be_pushint(s_vm, 1);
        be_getindex(s_vm, -2);
        be_remove(s_vm, -2); /* remove key; stack: [..., list, pair, fn] */

        if (!be_isfunction(s_vm, -1)) {
            be_pop(s_vm, 2); /* fn + pair */
            continue;
        }
        /* Call fn(topic, payload). Stack before pcall: [..., list, pair, fn, topic, payload] */
        be_pushnstring(s_vm, topic, topic_len);
        be_pushnstring(s_vm, payload, payload_len);
        int rc = be_pcall(s_vm, 2);
        if (rc != 0) {
            s_errors_total++;
            const char *msg = be_tostring(s_vm, -1);
            log_event("subscribe cb error: %s", msg ? msg : "?");
        }
        /* be_pcall leaves the function + args + result on the stack.
         * Clean back to [..., list, pair]. */
        be_pop(s_vm, be_top(s_vm) - top - 2); /* leave list + pair */
        be_pop(s_vm, 1); /* pair */
    }
    be_pop(s_vm, 1); /* list */
}

static void run_autoexec(void)
{
    bool any_enabled = false;
    for (int s = 0; s < BERRY_SLOT_COUNT; s++) {
        if (!nvs_slot_get_enabled(s)) continue;
        char *script = (char *)malloc(BERRY_SCRIPT_MAX + 1);
        if (!script) { log_event("slot %d: malloc failed", s); continue; }
        size_t n = nvs_slot_get_script(s, script, BERRY_SCRIPT_MAX + 1);
        if (n == 0) { free(script); continue; }
        any_enabled = true;
        char label[BERRY_LABEL_MAX] = "";
        nvs_slot_get_label(s, label, sizeof(label));
        if (!label[0]) snprintf(label, sizeof(label), "slot%d", s);
        log_event("autoexec[%d] \"%s\" running (%u bytes)", s, label, (unsigned)n);
        char result[160];
        bool ok = run_source(script, result, sizeof(result));
        if (ok) log_event("autoexec[%d] ok", s);
        free(script);
    }
    if (!any_enabled) log_event("autoexec: no enabled slots");
}

static void berry_task(void *arg)
{
    (void)arg;
    vm_construct();
    /* Sanity smoke-test so we know the VM is alive even before
     * any user script runs. Avoid sys.version() — that lives in an
     * embedded .be source our stripped build doesn't compile in. */
    char boot_out[80] = {0};
    run_source("import math\nprint('berry up, pi=' + str(math.pi))",
               boot_out, sizeof(boot_out));
    run_autoexec();

    bcmd_t *c;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE || !c) continue;
        switch (c->kind) {
        case BCMD_EVAL:
            c->ok = run_source(c->src, c->result, c->result_sz);
            xSemaphoreGive(c->done);
            break;
        case BCMD_RESTART:
            log_event("restart requested");
            vm_construct();
            run_autoexec();
            c->ok = (s_vm != NULL);
            xSemaphoreGive(c->done);
            break;
        case BCMD_LOAD_AUTOEXEC:
            run_autoexec();
            c->ok = true;
            xSemaphoreGive(c->done);
            break;
        case BCMD_TOPIC:
            dispatch_topic(c->topic, c->topic_len,
                           (const char *)c->payload, c->payload_len);
            free(c->topic);
            free(c->payload);
            free(c);   /* topic events are heap-allocated bcmd_t */
            break;
        }
    }
}

void berry_publish_topic_event(const char *topic, size_t topic_len,
                               const uint8_t *payload, size_t payload_len)
{
    /* Fast skip: no listeners, or queue not constructed yet. */
    if (s_topic_subs_count <= 0 || !s_cmd_q) return;
    /* Allocate a self-contained event. We copy topic + payload so the
     * broker's buffers can be reused immediately. */
    bcmd_t *c = (bcmd_t *)calloc(1, sizeof(*c));
    if (!c) { s_topic_events_dropped++; return; }
    c->kind = BCMD_TOPIC;
    c->topic = (char *)malloc(topic_len + 1);
    c->payload = (uint8_t *)malloc(payload_len + 1);
    if (!c->topic || !c->payload) {
        free(c->topic); free(c->payload); free(c);
        s_topic_events_dropped++;
        return;
    }
    memcpy(c->topic, topic, topic_len);   c->topic[topic_len] = '\0';
    if (payload_len > 0) memcpy(c->payload, payload, payload_len);
    c->payload[payload_len] = '\0';
    c->topic_len = topic_len;
    c->payload_len = payload_len;

    /* Non-blocking send. broker_task must never stall on a busy script. */
    if (xQueueSend(s_cmd_q, &c, 0) != pdTRUE) {
        free(c->topic); free(c->payload); free(c);
        s_topic_events_dropped++;
        return;
    }
    s_topic_events_total++;
}

bool berry_init(void)
{
    if (s_cmd_q) return true;
    migrate_legacy_slot();  /* one-time: move berry_en/berry_script → slot 0 */
    s_log_mux = xSemaphoreCreateMutex();
    /* 32-deep so topic-event bursts don't starve eval/restart commands
     * or cause the broker to drop fanout events under normal MQTT load. */
    s_cmd_q   = xQueueCreate(32, sizeof(bcmd_t *));
    if (!s_log_mux || !s_cmd_q) {
        ESP_LOGE(TAG, "berry_init: queue/mutex alloc failed");
        return false;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(berry_task, "berry", 8192, NULL,
                                            tskIDLE_PRIORITY + 2, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "berry_init: task create failed");
        return false;
    }
    return true;
}

static bool submit_cmd(bcmd_t *c, int timeout_ms)
{
    if (!s_cmd_q) return false;
    c->done = xSemaphoreCreateBinary();
    if (!c->done) return false;
    if (xQueueSend(s_cmd_q, &c, pdMS_TO_TICKS(50)) != pdTRUE) {
        vSemaphoreDelete(c->done);
        return false;
    }
    if (xSemaphoreTake(c->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* On timeout we leak the semaphore (rare path) rather than
         * risk use-after-free if the task finishes later. */
        return false;
    }
    vSemaphoreDelete(c->done);
    return c->ok;
}

bool berry_eval(const char *src, char *out_buf, size_t out_buf_sz, int timeout_ms)
{
    if (!src || !out_buf || out_buf_sz == 0) return false;
    bcmd_t c = {
        .kind = BCMD_EVAL,
        .src = src,
        .result = out_buf,
        .result_sz = out_buf_sz,
    };
    bool ok = submit_cmd(&c, timeout_ms);
    if (!ok && out_buf[0] == '\0') {
        snprintf(out_buf, out_buf_sz, "eval timeout or busy");
    }
    return ok;
}

/* ---- Multi-slot public API ---- */

bool berry_restart(void)
{
    if (!s_cmd_q) return false;
    bcmd_t c = { .kind = BCMD_RESTART };
    return submit_cmd(&c, 2000);
}

bool berry_slot_save(int slot, const char *label, size_t label_len,
                     const char *script, size_t script_len, bool enabled)
{
    if (slot < 0 || slot >= BERRY_SLOT_COUNT) return false;
    bool ok = true;
    if (label)  ok &= nvs_slot_set_label(slot, label, label_len);
    if (script) ok &= nvs_slot_set_script(slot, script, script_len);
    nvs_slot_set_enabled(slot, enabled);
    log_event("slot %d saved (%u bytes, en=%d)", slot,
              (unsigned)(script ? script_len : 0), enabled ? 1 : 0);
    berry_restart();
    return ok;
}

bool berry_slot_get(int slot, berry_slot_t *out)
{
    if (slot < 0 || slot >= BERRY_SLOT_COUNT || !out) return false;
    nvs_slot_get_label(slot, out->label, sizeof(out->label));
    out->enabled = nvs_slot_get_enabled(slot);
    out->script = (char *)malloc(BERRY_SCRIPT_MAX + 1);
    if (!out->script) { out->script_len = 0; return false; }
    out->script_len = nvs_slot_get_script(slot, out->script, BERRY_SCRIPT_MAX + 1);
    return true;
}

bool berry_slot_set_enabled(int slot, bool en)
{
    if (slot < 0 || slot >= BERRY_SLOT_COUNT) return false;
    nvs_slot_set_enabled(slot, en);
    return berry_restart();
}

/* ---- Legacy shims (slot 0) ---- */

bool berry_save_script(const char *src, size_t src_len)
{
    return berry_slot_save(0, NULL, 0, src, src_len,
                           nvs_slot_get_enabled(0));
}

size_t berry_get_script(char *buf, size_t bufsz)
{
    return nvs_slot_get_script(0, buf, bufsz);
}

void berry_set_enabled(bool en)
{
    nvs_slot_set_enabled(0, en);
    log_event("slot 0 enabled = %d", en ? 1 : 0);
    berry_restart();
}

bool berry_get_enabled(void)
{
    return nvs_slot_get_enabled(0);
}

void berry_get_status(berry_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* enabled = true if any slot is enabled */
    for (int s = 0; s < BERRY_SLOT_COUNT; s++) {
        if (nvs_slot_get_enabled(s)) { out->enabled = true; break; }
    }
    out->running = s_running;
    out->evals_total  = s_evals_total;
    out->errors_total = s_errors_total;
    /* script_len = total bytes across all enabled slots */
    char *tmp = (char *)malloc(BERRY_SCRIPT_MAX + 1);
    if (tmp) {
        for (int s = 0; s < BERRY_SLOT_COUNT; s++) {
            if (!nvs_slot_get_enabled(s)) continue;
            out->script_len += nvs_slot_get_script(s, tmp, BERRY_SCRIPT_MAX + 1);
        }
        free(tmp);
    }
    if (s_running && s_vm_start_us > 0)
        out->uptime_ms = (uint32_t)((esp_timer_get_time() - s_vm_start_us) / 1000);
}
