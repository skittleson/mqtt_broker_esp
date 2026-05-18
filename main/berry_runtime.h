/* Berry runtime — owns the VM, event queue, ring buffer, NVS state.
 *
 * Phase 1: VM up, REPL via berry_eval().
 * Phase 2 (this file): NVS persistence (mqtt_cfg/berry_en, berry_script),
 *                      autoexec on boot, berry_restart(), berry_save_script(),
 *                      berry_set_enabled(), heap accounting in status.
 *
 * Later phases add: berry_publish_event() for topic/lifecycle hooks,
 *                    heap cap + per-callback budget enforcement.
 */
#ifndef BERRY_RUNTIME_H
#define BERRY_RUNTIME_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of the persisted autoexec script. Chosen so that a single
 * NVS string entry stays comfortably below the 4000-byte per-entry limit
 * that NVS imposes by default. If users ever bump up against this, we
 * either split into multiple keys or move to a dedicated FS partition. */
#define BERRY_SCRIPT_MAX 3500

/* Initialize the Berry runtime. Reads `berry_en` + `berry_script` from
 * NVS namespace "mqtt_cfg". If enabled and script is non-empty, the
 * autoexec runs on the Berry task as soon as the VM is up. Always safe
 * to call: returns true even when scripting is disabled. */
bool berry_init(void);

/* Run a one-shot snippet on the Berry task. Blocks up to timeout_ms.
 * out_buf receives the printable result or an error message. Returns
 * true on success, false on parse/runtime error or timeout. */
bool berry_eval(const char *src, char *out_buf, size_t out_buf_sz,
                int timeout_ms);

/* Persist a new autoexec script to NVS and restart the VM so the change
 * takes effect. Truncates at BERRY_SCRIPT_MAX. Returns true on success. */
bool berry_save_script(const char *src, size_t src_len);

/* Read the persisted autoexec script into caller-owned buf. Returns the
 * number of bytes written (not including the trailing NUL). */
size_t berry_get_script(char *buf, size_t bufsz);

/* Master enable: writes mqtt_cfg/berry_en. Setting to false stops the
 * VM (no callbacks fire, no autoexec on next boot); true re-arms it. */
void berry_set_enabled(bool en);
bool berry_get_enabled(void);

/* Tear down the VM and rebuild it from scratch. Re-runs autoexec if
 * enabled. Returns true on success. Use after save_script or whenever
 * the user wants a clean slate. Blocks the caller up to ~1 s. */
bool berry_restart(void);

/* Status snapshot. Cheap to call. */
typedef struct {
    bool      enabled;       /* berry_en NVS flag */
    bool      running;       /* berry_task is up and VM is constructed */
    size_t    heap_used;     /* not wired until P7's allocator hook */
    unsigned  evals_total;   /* counter of berry_eval invocations */
    unsigned  errors_total;  /* counter of compile or runtime errors */
    size_t    script_len;    /* current autoexec length in bytes */
    uint32_t  uptime_ms;     /* ms since VM constructed */
} berry_status_t;

void berry_get_status(berry_status_t *out);

/* Snapshot the last N log/stdout lines into a caller-owned buffer.
 * Lines are newline-separated. Returns bytes written (not including
 * the trailing NUL). */
size_t berry_log_snapshot(char *buf, size_t bufsz);

/* Broker -> Berry topic event hook (P3).
 *
 * Called from broker_task's fanout loop after the existing subs[] +
 * tester walks. Posts a {topic, payload} event to berry_task; the
 * actual `mqtt.subscribe` callbacks run on berry_task under the same
 * dispatch loop as eval/restart commands. Non-blocking; drops events
 * when the queue is full (the broker must never stall on a flaky
 * script). Cheap when no Berry subscriptions are armed. */
void berry_publish_topic_event(const char *topic, size_t topic_len,
                               const uint8_t *payload, size_t payload_len);

/* Fast-path skip used by the broker before allocating a topic event:
 * true if at least one mqtt.subscribe() callback is registered. */
bool berry_has_topic_subs(void);

/* Called from berry_mod_mqtt.c whenever the subscription list changes.
 * The runtime caches the count atomically so berry_has_topic_subs() is
 * a single load. */
void berry_set_topic_subs_count(int count);

#ifdef __cplusplus
}
#endif

#endif /* BERRY_RUNTIME_H */
