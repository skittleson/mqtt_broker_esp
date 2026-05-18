/* Berry runtime — Phase 1 + 2 + 3.
 *
 * P1:
 *   - Single Berry VM owned by berry_task on CPU 1.
 *   - berry_eval() for one-shot snippets via request/response queue.
 *   - be_writebuffer() captures into a ~4 KB ring buffer + ESP_LOG mirror.
 *
 * P2:
 *   - NVS persistence under "mqtt_cfg" (berry_en u8, berry_script str).
 *   - autoexec runs when berry_en && script_len > 0, immediately after
 *     the VM is constructed on first boot or restart.
 *   - berry_save_script / berry_set_enabled / berry_restart exposed for
 *     portal_berry.c.
 *
 * P3 (this revision):
 *   - berry_mod_mqtt_register() wired into vm_construct().
 *   - `mqtt` global available to scripts: subscribe/unsubscribe/publish.
 *   - berry_publish_topic_event() / berry_has_topic_subs() called from
 *     mqtt_broker.c::handle_publish_internal() after fanout.
 *   - dispatch_topic() iterates _be_mqtt_subs and fires matching callbacks.
 */
#include "berry_runtime.h"
#include "berry_mod_mqtt.h"

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

#define BERRY_NVS_NS    "mqtt_cfg"  /* shared with portal/timers; see AGENTS.md §4 */
#define BERRY_NVS_EN    "berry_en"
#define BERRY_NVS_SCRIPT "berry_script"

/* --- Ring buffer for stdout / log --- */
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

/* --- NVS persistence ---
 * The "mqtt_cfg" namespace is shared with portal.c + timers.c. We open
 * it directly here (same pattern as timers.c) rather than refactoring
 * portal.c's helpers into a shared module mid-feature.
 */
static bool nvs_load_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(BERRY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, BERRY_NVS_EN, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void nvs_store_enabled(bool en)
{
    nvs_handle_t h;
    if (nvs_open(BERRY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, BERRY_NVS_EN, en ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static size_t nvs_load_script(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return 0;
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(BERRY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = bufsz;
    esp_err_t err = nvs_get_str(h, BERRY_NVS_SCRIPT, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return 0;
    }
    /* NVS returns len including NUL; we want bytes-without-NUL. */
    return len > 0 ? len - 1 : 0;
}

static bool nvs_store_script(const char *src, size_t src_len)
{
    if (!src) src = "";
    if (src_len > BERRY_SCRIPT_MAX) src_len = BERRY_SCRIPT_MAX;
    /* Make a NUL-terminated copy because NVS expects a C string. */
    char *tmp = (char *)malloc(src_len + 1);
    if (!tmp) return false;
    memcpy(tmp, src, src_len);
    tmp[src_len] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(BERRY_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { free(tmp); return false; }
    err = nvs_set_str(h, BERRY_NVS_SCRIPT, tmp);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    free(tmp);
    return err == ESP_OK;
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
    if (!nvs_load_enabled()) {
        log_event("autoexec skipped (berry_en=0)");
        return;
    }
    char *script = (char *)malloc(BERRY_SCRIPT_MAX + 1);
    if (!script) {
        log_event("autoexec: malloc failed");
        return;
    }
    size_t n = nvs_load_script(script, BERRY_SCRIPT_MAX + 1);
    if (n == 0) {
        log_event("autoexec skipped (script empty)");
        free(script);
        return;
    }
    log_event("autoexec running (%u bytes)", (unsigned)n);
    char result[160];
    bool ok = run_source(script, result, sizeof(result));
    if (ok) {
        log_event("autoexec ok");
    }
    free(script);
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

bool berry_save_script(const char *src, size_t src_len)
{
    if (src_len > BERRY_SCRIPT_MAX) src_len = BERRY_SCRIPT_MAX;
    if (!nvs_store_script(src, src_len)) {
        log_event("save_script: NVS write failed");
        return false;
    }
    log_event("script saved (%u bytes)", (unsigned)src_len);
    return berry_restart();
}

size_t berry_get_script(char *buf, size_t bufsz)
{
    return nvs_load_script(buf, bufsz);
}

void berry_set_enabled(bool en)
{
    nvs_store_enabled(en);
    log_event("enabled = %d", en ? 1 : 0);
    berry_restart();
}

bool berry_get_enabled(void)
{
    return nvs_load_enabled();
}

bool berry_restart(void)
{
    if (!s_cmd_q) return false;
    bcmd_t c = { .kind = BCMD_RESTART };
    return submit_cmd(&c, 2000);
}

void berry_get_status(berry_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enabled = nvs_load_enabled();
    out->running = s_running;
    out->evals_total  = s_evals_total;
    out->errors_total = s_errors_total;
    char *tmp = (char *)malloc(BERRY_SCRIPT_MAX + 1);
    if (tmp) {
        out->script_len = nvs_load_script(tmp, BERRY_SCRIPT_MAX + 1);
        free(tmp);
    }
    if (s_running && s_vm_start_us > 0) {
        out->uptime_ms = (uint32_t)((esp_timer_get_time() - s_vm_start_us) / 1000);
    }
    /* heap_used filled in P7 when the allocator wrapper lands. */
}
