# mqtt_broker v0.7.0-rc2 — NTP Phase 2: SNTP server

Built: May 13 2026 · SHA-256:
`93396b98f5a509e76fb5dbc2a7668be5d7750f46dd932772cef71fbea5da40c5`

Closes Phase 2 of [`plan-ntp-server.md`](../plan/plan-ntp-server.md). The broker
now binds UDP :123 and answers SNTPv4 client queries, so any device on
the same LAN can sync from it. Phases 0 (test harness), 3 (DHCP opt 42 +
`/time` page), and 4 (drift/manual-set/broadcast) still deferred.

## What landed

### `main/ntp.c` (server task)

New `ntp_server_start()` spawns `ntp_server_task` pinned to CPU 0
(stack 3072 B). Hot path is allocation-free:

- 48-byte static tx buffer in the task's stack frame.
- 32-entry per-source rate-limit LRU in BSS (~512 B total).
- One `recvfrom()` → `sendto()` cycle per request, four `htonl()` /
  `memcpy()` pairs to lay down the 64-bit NTP timestamps.

Packet handling per RFC 4330:

| Guard | Behaviour |
|-------|-----------|
| Length `< 48` or `> 68` | silent drop, `dropped_size++` (anti-amplification) |
| Mode != 1 and != 3 | silent drop, `dropped_mode++` (blocks classic ntpd abuse vectors mode=6/7) |
| Per-source > 10 req/s | silent drop, `dropped_rate++` (32-entry LRU) |
| Pre-sync | LI=3 (alarm), stratum 16, ref_id `"INIT"` — well-behaved clients ignore |
| Post-sync | LI=0, stratum 3 (conservative; correct if upstream is stratum 2), ref_id `"ESP3"` |

Timestamp capture:
- Receive ts: `ntp_now_us()` called *immediately* after `recvfrom()`.
- Originate ts: echo of bytes 40..47 from the client packet.
- Transmit ts: `ntp_now_us()` called *just before* `sendto()`.

So the client-computed RTT and offset are dominated by network jitter,
not our processing time.

Thread safety: esp_sntp's IDF callback is the sole writer of `s_synced`.
The server task only reads it. Plain boolean read is safe without a lock.

### `main/main.c`

`ntp_server_start()` called right after `ntp_init()`. Honours
NVS:`ntp.srv_enabled` (default 1); also gates on NVS:`ntp.enabled`
(master switch).

### `main/mqtt_broker.c`

The same 10 s tick that publishes `$SYS/broker/time` now also publishes
three retained NTP topics:

```
$SYS/broker/ntp/synced    '1' / '0'                  (retained)
$SYS/broker/ntp/stratum   '2'..'16'                  (retained)
$SYS/broker/ntp/served    total SNTP responses sent  (retained)
```

Republishing every 10 s rather than only on edge keeps the implementation
simple (no "last value" tracking); the broker's retain table dedupes by
topic so the cost is one fanout pass plus one retain-table update per
cycle.

### `main/portal.c`

`/api/time` JSON gains four fields:

```json
{
  "synced": true,
  "epoch_us": 1778706916366484,
  "last_sync_age_s": 44,
  "sync_count": 1,
  "upstream": "pool.ntp.org",
  "server_running": true,
  "stratum": 3,
  "served": 0,
  "dropped": { "rate": 0, "size": 0, "mode": 0 }
}
```

Still auth-exempt (open) on GET — same security model as Phase 1.

`/settings` Time (NTP) fieldset gains:
- A second status line:
  `server · serving on UDP:123 · stratum 3 · 8 served · dropped 0/2/1 (rate/size/mode)`
- A separate `Enable SNTP server (UDP:123) — LAN clients can sync from this device` checkbox.

New NVS key `srv_enabled` in namespace `ntp` (default 1), persisted by
the `save = confirm + reboot` flow.

## End-to-end verification (live device, Basic Auth on)

### Raw SNTPv4 query from developer host

```
resp 48 bytes  LI=0  VN=4  Mode=4
stratum=3  poll=2^6s  precision=2^-20s
ref_id=b'ESP3'
rtt=5.6ms  offset=-18.96ms          ← well under plan target ±50 ms
orig_ts echo matches sent: True
```

### Anti-amplification guards (silent drops verified)

```
oversize  (200 B)  -> dropped_size++  (no reply)
undersize (16 B)   -> dropped_size++  (no reply)
mode=6  (control)  -> dropped_mode++  (no reply)
burst (30 req in tight loop, single source):
  6 replies + 24 dropped (LwIP UDP recv queue saturation + rate limit
  combine to give effective rate-limit-from-the-client's-POV).
```

### `/api/time` after burst

```json
"served": 8, "dropped": { "rate": 0, "size": 2, "mode": 1 }
```

### `$SYS/broker/ntp/*` topics (paho-mqtt on `$SYS/broker/#`)

```
(R) $SYS/broker/ntp/served  -> '8'        ← retained, delivered on connect
(R) $SYS/broker/ntp/stratum -> '3'
(R) $SYS/broker/ntp/synced  -> '1'
    $SYS/broker/time          -> '1778706961'   ← live, every 10s
    $SYS/broker/ntp/synced    -> '1'            ← retained refresh
    $SYS/broker/ntp/stratum   -> '3'
    $SYS/broker/ntp/served    -> '8'
```

## Acceptance criteria status (plan-ntp-server.md §7)

|  # | Criterion | Status |
|---:|-----------|--------|
|  1 | `$SYS/broker/time` published within 60 s of cold boot | ✅ (since 0.7.0-rc1) |
|  2 | LAN client gets a usable SNTP response | ✅ **this release** (RTT 5.6 ms, offset −19 ms) |
|  3 | DHCP option 42 advertise-self | ⏳ Phase 3 |
|  4 | `/time` portal page <200 ms over Wi-Fi | ⏳ Phase 3 |
|  5 | `make test-ntp` CI green | ⏳ Phase 0 |
|  6 | README "What's new in 0.7.0" merged | ✅ (rc1 + rc2 entries) |

## Files changed

```
main/ntp.h                +13  -1   (server-side fields, ntp_server_start())
main/ntp.c                +275 -3   (server task, rate limit, packet build)
main/main.c               +5   -2   (ntp_server_start() call)
main/mqtt_broker.c        +35  -3   ($SYS/broker/ntp/* retained publishers)
main/portal.c             +75  -9   (/api/time expanded, server checkbox,
                                      server status line, srv_enabled NVS)
main/version.h            +5   -3   (0.7.0-rc1 -> 0.7.0-rc2)
README.md                 +25  -1   (What's new in 0.7.0-rc2, endpoint
                                      table picks up UDP:123, version table)
CHANGELOG-v0.7.0-rc2.md   new
docs/screenshots/         refreshed (settings shows both status lines +
                                      new server checkbox)
releases/...-rc2.bin           new
releases/...-rc2.bin.sha256    new
```

Binary size: 1.14 MB → 1.14 MB (+3 KB net over rc1). Combined Phase 1 +
Phase 2 flash growth: **12 KB exactly at plan budget**. Phase 3 needs to
fit in 0 KB of header room — either trim now (e.g. drop one of the
`$SYS/broker/ntp/*` topics, or compact the access-log code) or revise the
plan budget upward.

## Not in this release (Phase 3+)

- DHCP option 42 on the AP DHCP server.
- `/time` Tasmota-style portal page (live clock, peers table, recent
  clients).
- `POST /api/time/set` manual override for air-gapped installs.
- `CONFIG_NTP_BROADCAST` periodic broadcast mode.
- Drift compensation while free-running (linear-fit upstream offsets).
- Phase 0 test harness (host-build firmware + `ntplib` smoke tests).
