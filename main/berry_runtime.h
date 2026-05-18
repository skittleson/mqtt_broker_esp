/* Berry runtime — owns the VM, event queue, ring buffer, NVS state.
 *
 * Phase 1: VM up, REPL via berry_eval().
 * Phase 2: NVS persistence, autoexec on boot, berry_restart().
 * Phase 3: topic-event hook, berry_publish_topic_event().
 * Multi-slot (this revision): 4 named script slots replacing the single
 *   berry_script key. Legacy single-slot data is migrated to slot 0 on
 *   first access. berry_save_script / berry_get_script kept as slot-0
 *   shims for callers that predate this change.
 */
#ifndef BERRY_RUNTIME_H
#define BERRY_RUNTIME_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of independent script slots. Each slot has a label, a script
 * body, and an enable flag. All enabled slots run sequentially in the
 * shared VM on boot/restart, in slot order. */
#define BERRY_SLOT_COUNT   4

/* Per-slot script body cap. Chosen so each NVS string key stays well
 * below the 4000-byte per-entry NVS limit. */
#define BERRY_SCRIPT_MAX   2000

/* Maximum label length (including NUL). */
#define BERRY_LABEL_MAX    32

/* NVS key names follow the pattern berry_sN_{nm,sc,en} where N in 0..3.
 * All keys stay under 15 chars (NVS hard limit). */

/* Per-slot data snapshot (caller-owned). */
typedef struct {
    char  label[BERRY_LABEL_MAX]; /* user-visible name, NUL-terminated    */
    char *script;                 /* heap-allocated, caller must free      */
    size_t script_len;            /* bytes, not counting NUL               */
    bool  enabled;                /* true → run at boot                    */
} berry_slot_t;

/* Initialize the Berry runtime. Migrates any legacy single-slot NVS data
 * (berry_en / berry_script) into slot 0 on first boot after upgrade.
 * If at least one slot is enabled and non-empty, autoexec runs on the
 * Berry task as soon as the VM is up. Always safe to call: returns true
 * even when all slots are disabled. */
bool berry_init(void);

/* Run a one-shot snippet on the Berry task. Blocks up to timeout_ms.
 * out_buf receives the printable result or an error message. Returns
 * true on success, false on parse/runtime error or timeout. */
bool berry_eval(const char *src, char *out_buf, size_t out_buf_sz,
                int timeout_ms);

/* ---- Multi-slot API ---- */

/* Save slot `slot` (0..BERRY_SLOT_COUNT-1): persist label, script, and
 * enabled flag to NVS. Restarts the Berry VM so changes take effect.
 * label may be NULL (keeps current label). Returns true on success. */
bool berry_slot_save(int slot, const char *label, size_t label_len,
                     const char *script, size_t script_len, bool enabled);

/* Read slot `slot` into caller-owned `out`. out->script is heap-allocated;
 * caller must free it. Returns true on success (even if slot is empty). */
bool berry_slot_get(int slot, berry_slot_t *out);

/* Enable or disable a single slot without touching its script/label.
 * Restarts the Berry VM. */
bool berry_slot_set_enabled(int slot, bool en);

/* ---- Legacy shims (slot 0) ---- */

/* Persist a new script to slot 0 and restart the VM. */
bool berry_save_script(const char *src, size_t src_len);

/* Read the slot-0 script into caller-owned buf. */
size_t berry_get_script(char *buf, size_t bufsz);

/* Master enable for slot 0. */
void berry_set_enabled(bool en);
bool berry_get_enabled(void);

/* Tear down the VM and rebuild it from scratch. Re-runs all enabled
 * slots in order. Returns true on success. Blocks up to ~1 s. */
bool berry_restart(void);

/* Status snapshot. Cheap to call. */
typedef struct {
    bool      enabled;        /* true if any slot is enabled               */
    bool      running;        /* berry_task is up and VM is constructed    */
    size_t    heap_used;      /* not wired until P7's allocator hook       */
    unsigned  evals_total;    /* counter of berry_eval invocations         */
    unsigned  errors_total;   /* counter of compile or runtime errors      */
    size_t    script_len;     /* total bytes across all enabled slots      */
    uint32_t  uptime_ms;      /* ms since VM constructed                   */
} berry_status_t;

void berry_get_status(berry_status_t *out);

/* Snapshot the last N log/stdout lines into a caller-owned buffer.
 * Lines are newline-separated. Returns bytes written (not including NUL). */
size_t berry_log_snapshot(char *buf, size_t bufsz);

/* Broker -> Berry topic event hook (P3). Non-blocking. */
void berry_publish_topic_event(const char *topic, size_t topic_len,
                               const uint8_t *payload, size_t payload_len);

/* Fast-path skip: true if at least one mqtt.subscribe() callback is armed. */
bool berry_has_topic_subs(void);

/* Called from berry_mod_mqtt.c whenever the subscription list changes. */
void berry_set_topic_subs_count(int count);

#ifdef __cplusplus
}
#endif

#endif /* BERRY_RUNTIME_H */
