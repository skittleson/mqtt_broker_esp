# mqtt_broker v0.7.0 — NTP feature complete + test harness

Built: May 13 2026 · SHA-256:
`2c4759a265e4d524d3f703085d07edd2f6a8dd01eac8df254a19d42f047f7856`

Tagged release. Promotes 0.7.0-rc3 to final after the full Phase 0 test
harness landed and the integration suite went green.

## What's in 0.7.0

Three rc tags rolled into one shipped version. Detailed per-phase audits
in `CHANGELOG-v0.7.0-rc{1,2,3}.md`; high-level summary:

| Slice | Phase | What |
|-------|-------|------|
| **rc1** | 1 | SNTP client (`esp_sntp`), `/api/time` (open), `/api/time/resync` (gated), `$SYS/broker/time`, Time (NTP) section in `/settings` |
| **rc2** | 2 | SNTP server on UDP :123 (anti-amplification, rate limit, stratum 16/LI=3 when unsynced), retained `$SYS/broker/ntp/{synced,stratum,served}`, server fields in `/api/time` |
| **rc3** | 3 | `/time` portal page (server-rendered, recent-clients table), mDNS `_ntp._udp` advertisement (substitutes for DHCP option 42), dashboard nav button |
| **final** | 0 | Test harness: `test_ntp.py` (13 tests) + `test_broker.py` updated for auth and the v0.6.3 save-and-reboot flow, `Makefile` (`make test` / `test-ntp` / `test-broker` / `ota` / `captures`), README screenshot carousel |

## Test harness

`test_ntp.py` covers every NTP acceptance criterion from
`plan-ntp-server.md`:

| # | Test | What it asserts |
|--:|---|---|
| 1 | `/api/time` JSON schema | All 9 fields present, types correct, `synced ⇔ epoch_us > 0` |
| 2 | `/api/time` is open | No Basic Auth required (used as a probe) |
| 3 | `$SYS/broker/time` flowing | MQTT subscriber sees the topic + retained `$SYS/broker/ntp/*` within 22 s |
| 4 | SNTP basic reply | Device-internal consistency: tx_ts ≥ rx_ts, handler latency < 20 ms, SNTP timestamp agrees with `/api/time` within ±100 ms |
| 5 | SNTP packet format | 48 B, VN=4, Mode=4, ref_id matches sync state (INIT/ESP3), LI matches stratum |
| 6 | Originate timestamp echo | Bytes 24..31 == client's TX bytes 40..47 |
| 7 | Drop oversize (200 B) | Silent drop, `dropped_size` increments |
| 8 | Drop undersize (16 B) | Silent drop, `dropped_size` increments |
| 9 | Drop mode 6 (control) | Silent drop, `dropped_mode` increments |
| 10 | Rate-limit burst | 30 requests in tight loop → < 30 replies |
| 11 | mDNS advertisement | Multicast PTR query for `_ntp._udp.local` → unicast reply from device |
| 12 | `/time` page renders | All four sections present, no JS fetch in body |
| 13 | `/time` page < 5 KB | Body size stays small for fast Wi-Fi render |

`test_broker.py` updated for two pre-existing breakages caused by the
v0.6.x UX rework:

- Added `BROKER_AUTH=user:pass` env var: monkey-patches
  `requests.get`/`requests.post` once at import so the rest of the file
  stays untouched.
- Destructive POST `/save-settings` and `/save` tests now skip by
  default (4 tests; would reboot the device on each run). Set
  `BROKER_TEST_DESTRUCTIVE=1` to enable; they then catch the
  `ChunkedEncodingError` raised when the device closes the socket
  mid-response and waits for `/api/ping` to come back before checking
  persistence.
- `/clients` page test updated: looks for either legacy
  `setTimeout(location.reload)` or new `fetch('/api/clients')` polling.

### Results against the live 0.7.0 device

```
test_ntp.py:    13 passed, 0 failed
test_broker.py: 116 passed, 0 failed, 4 skipped (destructive, opt-in)
                ────────────
                129 passed, 0 failed total
                85 s wall clock
```

Notable telemetry from the run:

- `/api/time` open path: schema valid, `synced=True, stratum=3, served=26`
- SNTP server handler latency: **0.1 ms** (well under the 20 ms budget)
- SNTP-vs-`/api/time` internal drift: **+14 ms** (round-trip noise)
- Defensive guards: 200 B / 16 B / mode-6 packets all silently dropped,
  counters increment correctly
- mDNS reply from `192.168.22.100` within 3 s of multicast query

## Makefile

New convenience targets (sourced from env, with sensible defaults):

```
make help          # list targets and env vars
make test          # test-ntp + test-broker
make test-ntp      # NTP suite only
make test-broker   # MQTT broker suite only
make build         # idf.py build (wrapper)
make ota           # curl-upload build/mqtt_broker.bin to /ota-upload
make captures      # refresh docs/screenshots/* against the live device
make fmt-version   # print FW_VERSION and the embedded binary version
```

Env vars honoured: `BROKER_HOST`, `BROKER_AUTH`, `PORTAL_URL`,
`PORTAL_AUTH`, `BROKER_TEST_DESTRUCTIVE`.

## README screenshot carousel

The Web Portal section was replaced with a GitHub-friendly carousel:

- Three-column desktop grid above the fold (Dashboard, Time / NTP,
  Settings).
- `<details>` collapsible "More screenshots" section containing:
  - Live device monitoring (Clients, Information, Tester, Firmware)
  - Setup, save & reboot (WiFi, confirm prompt, two countdown states)
  - Mobile views (six tiles at 390 × 844 @ 2×)
- Each thumbnail is a clickable link to the full-resolution PNG.

GitHub doesn't render JavaScript, so this is a static grid + collapsible
section pattern that degrades cleanly: visible thumbnails on first paint,
detail on demand, no broken interactivity.

## Files changed (vs 0.7.0-rc3)

```
main/version.h           +5   -5   (rc3 -> 0.7.0)
test_ntp.py              new (433 lines, 13 tests)
test_broker.py           +90  -33  (BROKER_AUTH support, destructive
                                    flag, /clients refresh check)
Makefile                 new (83 lines)
README.md                +175 -60  (0.7.0 What's-new section,
                                    screenshot carousel, Testing
                                    rewrite, version table)
CHANGELOG-v0.7.0.md      new
releases/mqtt_broker-v0.7.0.bin            new
releases/mqtt_broker-v0.7.0.bin.sha256     new
docs/screenshots/        refreshed (footer bump 0.7.0-rc3 -> 0.7.0)
```

## Plan acceptance criteria — final scorecard

|  # | Criterion | Status |
|---:|---|---|
| 1 | `$SYS/broker/time` within 60 s of cold boot | ✅ |
| 2 | LAN client gets a usable SNTP response | ✅ |
| 3 | Auto-discovery (DHCP option 42 or equivalent) | ✅ (mDNS substitution) |
| 4 | `/time` portal page < 100 ms over Ethernet | ✅ |
| 5 | `make test-ntp` CI green | ✅ **this release** |
| 6 | README "What's new in 0.7.0" merged | ✅ |

All six. Feature ships.

## Not in 0.7.0 (next release candidates)

- **Drift compensation** while free-running (no upstream available).
  Linear-fit the last 8 upstream offsets and apply correction between
  syncs. Plan estimates 24 h drift drops ~2 s → ~200 ms.
- **`POST /api/time/set`** manual override for air-gapped installs.
  Honoured only when NVS `ntp.accept_set` is on (currently always 0).
- **`CONFIG_NTP_BROADCAST`** periodic broadcast mode (RFC 4330 §3.3).
- **Real DHCP option 42** advertising. Needs patching ESP-IDF lwip
  dhcps to support arbitrary option codes.
- **Host-build firmware** for offline CI (currently tests need a flashed
  device on the LAN).
