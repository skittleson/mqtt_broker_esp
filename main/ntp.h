/*
 * NTP / SNTP integration.
 *
 * Phase 1 of plan-ntp-server.md: SNTP client only. Provides the rest of the
 * firmware with absolute wall-clock time once at least one upstream NTP
 * server responds. The companion SNTP server (Phase 2) ships separately.
 *
 * Public surface is intentionally minimal:
 *
 *   ntp_init()        Starts the SNTP client task using upstreams from NVS.
 *                     Safe to call before any netif is up; esp_sntp internally
 *                     waits for a default route before sending queries.
 *
 *   ntp_now_us()      Returns Unix epoch microseconds when ntp_is_synced(),
 *                     otherwise returns 0. Callers that need a monotonic
 *                     timestamp should continue using esp_timer_get_time();
 *                     ntp_now_us() is for *wall clock* values (UI labels,
 *                     $SYS/broker/time, retained-message birth times).
 *
 *   ntp_is_synced()   True once at least one upstream has answered. Stays
 *                     true across upstream loss -- the kernel's monotonic
 *                     clock keeps wall-clock time advancing at the last
 *                     observed offset. Reset only by reboot.
 *
 *   ntp_get_state()   Snapshot of {synced, last_sync_age_s, upstream used,
 *                     stratum hint} for the /api/time JSON endpoint and the
 *                     /settings page.
 *
 *   ntp_force_resync() Triggers an immediate upstream poll. Used by
 *                      POST /api/time/resync. Non-blocking; the answer
 *                      arrives in the SNTP callback some hundreds of ms
 *                      later.
 *
 * NVS namespace `ntp` is owned exclusively by this component (see
 * plan-ntp-server.md "NVS schema"). The portal reads keys to render the
 * settings page and writes them on /save-settings; ntp_init() picks them
 * up at boot.
 */

#ifndef NTP_H
#define NTP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max length of an upstream hostname incl. NUL. Matches the form input
 * size on /settings. Three slots advertised in the plan. */
#define NTP_UPSTREAM_MAX_LEN   64
#define NTP_UPSTREAMS_MAX      3

/* Snapshot returned to /api/time. Fields chosen to match the plan's
 * documented endpoint contract. `epoch_us == 0` <=> not synced. */
typedef struct {
    int64_t  epoch_us;          /* Wall-clock now in epoch microseconds, 0 if unsynced. */
    bool     synced;            /* Convenience mirror of (epoch_us != 0). */
    int64_t  last_sync_us;      /* When the last upstream reply landed (esp_timer units). */
    int64_t  last_sync_age_s;   /* Seconds since last_sync_us, or -1 if never. */
    char     upstream_used[NTP_UPSTREAM_MAX_LEN]; /* Hostname of the responder. */
    uint32_t sync_count;        /* Total successful upstream syncs since boot. */
    bool     server_running;    /* False for Phase 1 -- placeholder for Phase 2. */
} ntp_state_t;

/* Settings struct used by the portal to render the form and persist edits.
 * The portal reads NVS itself for individual fields; this struct is the
 * shape an /api/time/settings or similar bulk endpoint would return. Not
 * used internally beyond ntp_get_settings() for display. */
typedef struct {
    bool     enabled;
    uint32_t poll_s;
    char     tz[64];
    char     upstreams[NTP_UPSTREAMS_MAX][NTP_UPSTREAM_MAX_LEN];
} ntp_settings_t;

/* Lifecycle. Idempotent: calling more than once is a no-op. */
void ntp_init(void);

/* Wall-clock accessors. */
int64_t ntp_now_us(void);
bool    ntp_is_synced(void);

/* Read-only state for the portal. Pass NULL to ignore a field. */
void ntp_get_state(ntp_state_t *out);
void ntp_get_settings(ntp_settings_t *out);

/* Forces an immediate upstream poll. Returns false if NTP is disabled in
 * NVS or no netif has a default route. The result of the poll arrives
 * asynchronously some hundreds of ms later via the sync callback. */
bool ntp_force_resync(void);

#ifdef __cplusplus
}
#endif

#endif /* NTP_H */
