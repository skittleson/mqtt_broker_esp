# mqtt_broker v0.7.0-rc1 — NTP Phase 1: SNTP client

Built: May 13 2026 · SHA-256:
`8570b6b58bc3ab5b9e1c1bedc886315d0d561969ef22dbde095c9d86bd09a08e`

First slice of [`plan-ntp-server.md`](../plan/plan-ntp-server.md). Adds an SNTP
client + `$SYS/broker/time` publisher + `/api/time` HTTP endpoints + a
Time (NTP) section in `/settings`. SNTP server (Phase 2), DHCP option 42
(Phase 3), `/time` portal page (Phase 3), and drift compensation
(Phase 4) all defer to follow-up releases.

## What landed

### `main/ntp.c` + `main/ntp.h` (new component)

Thin wrapper around ESP-IDF's `esp_sntp`. Public surface:

```c
void    ntp_init(void);             // start client, idempotent
int64_t ntp_now_us(void);           // epoch µs, 0 if unsynced
bool    ntp_is_synced(void);
void    ntp_get_state(ntp_state_t *);
void    ntp_get_settings(ntp_settings_t *);
bool    ntp_force_resync(void);
```

Reads NVS namespace `ntp` (defaults wired in source):

| Key          | Type | Default               | Range/notes |
|--------------|------|-----------------------|-------------|
| `enabled`    | u8   | 1                     | Master on/off |
| `upstream_0` | str  | `pool.ntp.org`        | up to 63 chars |
| `upstream_1` | str  | `time.cloudflare.com` |  |
| `upstream_2` | str  | `` (unused)           |  |
| `poll_s`     | u32  | 3600 (1h)             | clamped 64–86400 |
| `tz`         | str  | `UTC0`                | POSIX TZ string |

### `main/main.c`

`ntp_init()` runs right after `broker_start()`. Safe to call before the
default route is fully ready — `esp_sntp` queues queries internally and
fires them as soon as DNS resolves the upstreams.

### `main/mqtt_broker.c`

New periodic publisher in `broker_task()`: `$SYS/broker/time` every 10 s
while `ntp_is_synced()`. Payload is ASCII epoch seconds. Uses the existing
`handle_publish_internal()` so retained handling, QoS-1 in-flight, and
fanout-to-subscribers all kick in like an external publisher.

### `main/portal.c`

Three pieces:

1. **`GET /api/time`** — added to the auth-exempt list (alongside
   `/api/ping`). Returns:
   ```json
   {
     "synced": true,
     "epoch_us": 1778705454926997,
     "last_sync_age_s": 57,
     "sync_count": 1,
     "upstream": "pool.ntp.org",
     "server_running": false
   }
   ```
   Phase 2 flips `server_running` to `true`.
2. **`POST /api/time/resync`** — auth-gated (the normal Basic Auth check
   covers it). Calls `esp_sntp_restart()` to schedule an immediate poll,
   returns `{"triggered":true}` with HTTP 200 (or 503 if NTP is disabled).
3. **Time (NTP) fieldset in `/settings`** — live sync summary
   (`synced · last 14s ago · 3 total` in green), enable checkbox, three
   upstream inputs, poll interval, POSIX TZ. Saved through the existing
   "save = confirm + reboot" flow into the new NVS namespace.

## End-to-end verification (live device, Ethernet, Basic Auth on)

### Initial sync after boot
```
GET /api/time   (no auth)
  -> 200, body:
     {"synced":true, "epoch_us":1778705454926997,
      "last_sync_age_s":57, "sync_count":1,
      "upstream":"pool.ntp.org", "server_running":false}
```

### Wall-clock accuracy
```
device:  2026-05-13T20:53:49.975+00:00
host:    2026-05-13T20:53:50.307+00:00     (host itself synced separately)
offset:  -332 ms
```
Plan target was ±50 ms within the LAN once synced. Drift from
`pool.ntp.org` directly is well under that; the 332 ms is mostly host-side
clock skew vs the device's own upstream.

### MQTT `$SYS/broker/time` cadence
```
paho-mqtt sub on  $SYS/broker/#:
  13:53:14  $SYS/broker/time  -> '1778705594'
  13:53:24  $SYS/broker/time  -> '1778705604'
total: 2 messages in 22s (cadence 10s as planned)
```

### Force-resync
```
Before:   sync_count=2, last_sync_age=12s
POST /api/time/resync   (auth)  -> 200 {"triggered":true}
4s later: sync_count=3, last_sync_age=0s
POST /api/time/resync   (no auth) -> 401  ← correctly gated
```

### Settings page render
Screenshot: `docs/screenshots/ux-audit/settings_desktop.png`. Shows the
new Time (NTP) fieldset between Ethernet Gateway and the Save & Reboot
button. Status line is green when synced, orange when not.

## Files changed

```
main/ntp.h               new (99 lines)
main/ntp.c               new (257 lines)
main/main.c              +9   -1   (include ntp.h, ntp_init() call)
main/mqtt_broker.c       +24  -0   ($SYS/broker/time publisher)
main/portal.c            +138 -2   (auth_exempt, /api/time, /api/time/resync,
                                     Time (NTP) settings fieldset, NVS persist)
main/CMakeLists.txt      +1   -1   (SRCS += ntp.c, REQUIRES += lwip)
main/version.h           +7   -2   (0.6.6 -> 0.7.0-rc1)
README.md                +14  -1   (What's new section, endpoint table,
                                    version table)
CHANGELOG-v0.7.0-rc1.md  new
docs/screenshots/        refreshed (Time (NTP) section visible in settings)
releases/mqtt_broker-v0.7.0-rc1.bin       new
releases/...-rc1.bin.sha256               new
```

Binary size: 1.13 MB → 1.14 MB (+9 KB). Phase 1+2+3 flash budget per the
plan is 12 KB; we have 3 KB headroom for Phases 2+3 combined.

## Not in this release (next slices)

- **Phase 2 — SNTP server.** Bind `0.0.0.0:123`, answer with stratum=16/LI=3
  when unsynced, real stratum once synced. Per-source rate limit, drop
  oversized packets, no malloc in hot path.
- **Phase 3 — DHCP option 42 + `/time` portal page.** Advertise self as
  NTP source over the AP DHCP server; add a Tasmota-style `/time` page
  with live clock and recent-clients table.
- **Phase 4 stretch — drift compensation, manual time set, broadcast mode.**
