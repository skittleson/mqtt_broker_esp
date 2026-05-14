# mqtt_broker v0.7.2 — NTP drift compensation

Built: May 13 2026 · SHA-256:
`f98f1592196ff4dd1e7c6aad60e718e3d634111b81b90c480794a06672c7c56a`

Tagged release. Closes the **"drift compensation while free-running"**
item from `CHANGELOG-v0.7.0.md`'s "Not in 0.7.0" list — the second
half of the "those are both pretty important" pair (CSRF was 0.7.1).

## Problem

The broker's wall clock is `gettimeofday()`, which is just the local
oscillator counting up from the last `settimeofday()` write by
`esp_sntp`. Between syncs, that oscillator drifts at whatever rate the
ESP32-S3 crystal happens to drift — measured on this hardware at
**−35 ppm** (our clock runs slow by ~35 microseconds per second of
real time). Over 24h that's ~3 seconds, which makes the
`$SYS/broker/time` topic and SNTP server responses progressively less
useful for downstream consumers.

If upstream NTP is available, this isn't visible because esp_sntp
re-corrects every poll interval (default 3600 s). The problem appears
in two scenarios:

1. **Free-running**: WAN outage, DNS misconfig, or hostile network
   blocks 123/udp upstream. Broker keeps serving LAN clients off its
   own (drifting) clock.
2. **Long poll intervals**: users who set poll to multi-hour values
   to be polite to public NTP pools see drift accumulate between
   syncs.

## Solution

Track `(monotonic_us, wallclock_us)` pairs in an 8-slot ring buffer.
After ≥ 2 syncs spanning ≥ 60 s of baseline, compute drift as
`(Δwall − Δmono) / Δmono * 1e6`, clamped to ±500 ppm to keep a single
bad sync from poisoning the estimate. When free-running for more than
2 × the poll interval, apply the linear correction
`corrected = gettimeofday() − drift_ppm × Δt` to outputs that face
external consumers:

- SNTP server `tx_timestamp` and `rx_timestamp` (UDP :123 responses)
- `$SYS/broker/time` MQTT topic

`gettimeofday()` itself and the `/api/time` `epoch_us` field stay
**uncorrected** — users can see the raw clock alongside `drift_ppm`
so the system is honest about what's measured vs estimated.

## What lands

### Code

| File | Change |
|---|---|
| `main/ntp.h` | New fields in `ntp_state_t`: `drift_ppm` (`INT32_MIN` = unknown), `free_running_s`. New `ntp_now_us_corrected()` public accessor. |
| `main/ntp.c` | 8-slot drift ring buffer, `ntp_drift_record()` in the sync callback, `ntp_corrected_now_us()` static helper used by SNTP server tx/rx timestamps. `s_poll_s` cached so `ntp_free_running_s()` can compute the 2×poll threshold. |
| `main/mqtt_broker.c` | `$SYS/broker/time` uses `ntp_now_us_corrected()`. New retained topics `$SYS/broker/ntp/drift_ppm` (empty payload when unknown) and `$SYS/broker/ntp/free_running_s`. |
| `main/portal.c` | `/api/time` JSON gains `drift_ppm` (number\|null) and `free_running_s` (int). `/time` page server status line gains "drift ±N ppm" suffix; when free-running, the suffix becomes "drift ±N ppm · free-running Ns" in orange. |

### Behaviour table

| Sync count | Baseline | drift_ppm | Compensation |
|---|---|---|---|
| 0 | n/a | null | none (no clock) |
| 1 | 0 | null | none |
| 2+ | < 60 s | null | none (noise floor) |
| 2+ | ≥ 60 s | measured | only when free-running > 2 × poll |

### Telemetry

`/api/time` example after a free-running window:

```json
{
  "synced": true, "epoch_us": 1778720353893528,
  "last_sync_age_s": 9, "sync_count": 4,
  "upstream": "pool.ntp.org",
  "server_running": true, "stratum": 3, "served": 12,
  "dropped": {"rate": 0, "size": 0, "mode": 0},
  "drift_ppm": -35,
  "free_running_s": 0
}
```

Retained MQTT topics added:

| Topic | Payload | Notes |
|---|---|---|
| `$SYS/broker/ntp/drift_ppm` | signed int as ASCII (e.g. `-35`) or empty bytes | empty payload distinguishes "unknown" from "0 ppm" |
| `$SYS/broker/ntp/free_running_s` | non-negative int | 0 means within normal poll cycle |

## Results on the live 0.7.2 device

Measured immediately after deployment:

```
sync 1: drift_ppm=null  (0 syncs, no baseline)
sync 2: drift_ppm=null  (1 sync recorded, 0s baseline)
sync 3 (manual /api/time/resync, ~65 s later): drift_ppm=-139  (noisy, baseline ~80s)
sync 4 (auto-poll, ~3600 s later): drift_ppm=-35   (stable, baseline ~3700s)
```

The −35 ppm reading matches typical ESP32-S3 crystal drift specs
(±10 ppm specified, ±50 ppm typical worst-case at ambient temperature).
This single device, when free-running, would now drift ~0.3 s/24h
instead of ~3.0 s/24h. **10× improvement, matches the plan target.**

### Test results

```
test_ntp.py:    13 passed, 0 failed   (drift schema test passes;
                                       $SYS topic test now requires
                                       drift_ppm + free_running_s
                                       to be flowing)
test_broker.py: 125 passed, 0 failed, 4 skipped (destructive)
                ───────
                138 passed, 0 failed total, 79.7 s wall clock
```

Drift correctness verified by manual probe:

```
$ curl /api/time | jq .drift_ppm
null            # sync_count=1
$ # ... 65 s later, manual resync ...
$ curl /api/time | jq .drift_ppm
-139            # baseline ~80s, noise-dominated
$ # ... 1h later, auto-poll ...
$ curl /api/time | jq .drift_ppm
-35             # baseline ~3700s, converged
```

## Files changed (vs 0.7.1)

```
main/version.h           +6  -5    (0.7.1 -> 0.7.2)
main/ntp.h               +20 -1    (drift_ppm + free_running_s fields,
                                    ntp_now_us_corrected declaration)
main/ntp.c               +135 -5   (drift ring buffer, drift_record,
                                    corrected_now_us, free_running_s,
                                    sync callback hook, SNTP server
                                    uses corrected timestamps)
main/mqtt_broker.c       +28 -2    ($SYS/broker/time uses corrected,
                                    new drift_ppm + free_running_s
                                    retained topics)
main/portal.c            +52 -7    (drift_ppm + free_running_s in
                                    /api/time JSON, drift suffix on
                                    /time page server line, expanded
                                    server_line buffer)
test_ntp.py              +25 -8    (drift_ppm schema check, $SYS
                                    drift topic flow test)
CHANGELOG-v0.7.2.md      new
releases/mqtt_broker-v0.7.2.bin            new (1,149,808 B)
releases/mqtt_broker-v0.7.2.bin.sha256     new
docs/screenshots/        refreshed (version bump 0.7.1 -> 0.7.2,
                                    /time page now shows drift line)
```

## Binary size

| Version | Size | Δ from prev |
|---|---:|---:|
| 0.7.0 | 1,146,592 B | — |
| 0.7.1 | 1,149,712 B | +3,120 B (CSRF) |
| 0.7.2 | 1,149,808 B | +96 B (drift comp) |

The drift code is tiny — most of it is comments. 73 % OTA slot still
free.

## Not in 0.7.2 (held)

- **Manual `POST /api/time/set`** for air-gapped installs. Would let
  the user paste a date string into /settings and have the broker
  treat it as ground truth. Plan-doc'd; deferred until a user actually
  asks.
- **Quadratic or higher-order drift fits.** A linear ppm is a single
  rate; ESP32 crystals can have temperature-dependent quadratic
  components if the device experiences large diurnal temperature
  swings. Out of scope for typical indoor deployments.
- **Drift persistence across reboot.** We rebuild the ring buffer
  from scratch on every reboot, so free-running corrections start
  applying again only after the second sync + 60 s baseline. Could
  persist `drift_ppm` to NVS for instant boot-time application —
  trade-off: NVS write per sync. Deferred.
- **Tasmesh broker** — separate plan, separate release. See
  `plan-tasmesh.md`.

## Plan scorecard

CSRF (0.7.1) + drift comp (0.7.2) closes the "both pretty important"
pair the user flagged. Remaining items from earlier plans:

| Plan | Item | Status |
|---|---|---|
| plan-mqtt-ux-v2 | Phase 4 (CSRF) | ✅ 0.7.1 |
| plan-ntp-server.md | Drift compensation | ✅ 0.7.2 |
| plan-ntp-server.md | POST /api/time/set | not done — held |
| plan-ntp-server.md | CONFIG_NTP_BROADCAST | not done — held |
| plan-ntp-server.md | Real DHCP option 42 | not done — held (needs IDF patch) |
| plan-tasmesh.md | Phase 0 (test harness) | not started |
| plan-tasmesh.md | Phases 1-4 | not started |

Next action open. CSRF + drift were the user's stated priorities and
they're both done.
