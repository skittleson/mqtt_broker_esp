# NTP Server Plan

Target firmware: `mqtt_broker` (post-0.6.4).
Goal: let any client on the same LAN (over Wi-Fi STA, Wi-Fi AP, or W5500
Ethernet) point at this ESP32-S3 and get accurate time over plain SNTPv4 —
**no extra service, no cloud, same plug-it-in promise as the MQTT broker.**

Companion to:

- `plan-mqtt-ux-v2.md` (UI/test conventions reused here)
- `docs/qos-persistence-plan.md` (NVS-settings + reboot-countdown conventions)

---

## Why

Today the broker has no notion of wall-clock time. That hurts in three
concrete ways:

1. MQTT `$SYS/broker/time` and per-client `connected_at` are uptime-based,
   so dashboards can't correlate broker events with anything else.
2. Retained-message timestamps (planned in `qos-persistence-plan.md`) need
   real epoch seconds to be useful across reboots.
3. Air-gapped deployments (factory floor, RV, boat, lab) frequently have
   **no internet** but do have devices that demand an NTP source on the
   LAN. Today users have to deploy a second box (Raspberry Pi, router with
   chrony) just to serve time. The ESP32 is already there — it should be
   the time source.

Non-goal: PTP, leap-second perfection, stratum-1 GPS. Target accuracy
**±50 ms within the LAN** once synced upstream, **±2 s free-running** for
up to 24 h after upstream loss (esp_timer drift bound on ESP32-S3 internal
RC + crystal).

---

## Architecture (one paragraph)

A new component `main/ntp.c` runs two cooperating pieces:

1. **SNTP client** (`esp_sntp` from ESP-IDF) that periodically pulls time
   from one to three configurable upstream servers — only attempted when
   either `STA_GOT_IP` or `ETH_GOT_IP` has fired and a default route
   exists. On first successful sync it records `boot_epoch_us` (the epoch
   that corresponds to `esp_timer_get_time() == 0`) into RAM and bumps the
   server's advertised **stratum** to `peer_stratum + 1` (clamped 2..15).
2. **SNTP server** — a single `lwip` UDP socket bound to `0.0.0.0:123` on
   all netifs, in its own ~3 KB FreeRTOS task. On every inbound packet it
   constructs a 48-byte SNTPv4 response per RFC 4330 §5, using
   `boot_epoch_us + esp_timer_get_time()` for `recv_ts` and `tx_ts`, the
   client's `tx_ts` echoed back as `orig_ts`, and stratum/precision/poll
   fields filled from the client state above. When unsynced it answers
   with **stratum 16 / LI=3 (alarm)** so well-behaved clients ignore it
   rather than locking onto bogus time — matches what chrony/ntpd do.

Everything else (portal page, NVS keys, DHCP option, MQTT `$SYS` topics)
is glue around those two.

---

## NVS schema (namespace `ntp`)

| Key            | Type  | Default               | Notes                                           |
| -------------- | ----- | --------------------- | ----------------------------------------------- |
| `enabled`      | `u8`  | `1`                   | Master on/off for both client + server.         |
| `srv_enabled`  | `u8`  | `1`                   | If 0, sync upstream but don't answer :123.      |
| `upstream_0`   | `str` | `pool.ntp.org`        | Up to 63 chars. Empty = unused.                 |
| `upstream_1`   | `str` | `time.cloudflare.com` |                                                 |
| `upstream_2`   | `str` | ``                    |                                                 |
| `poll_s`       | `u32` | `3600`                | Upstream poll interval, 64..86400.              |
| `tz`           | `str` | `UTC0`                | POSIX TZ string, e.g. `PST8PDT,M3.2.0,M11.1.0`. |
| `dhcp_opt42`   | `u8`  | `1`                   | Advertise self as NTP via AP DHCP option 42.    |
| `last_sync_us` | `u64` | `0`                   | Epoch-µs of most recent successful sync.        |
| `accept_set`   | `u8`  | `0`                   | If 1, accept `POST /api/time/set` (manual).     |

All editable through `/settings`. `last_sync_us` is written from code on
every successful upstream pull (rate-limited to once per `poll_s/4` to
spare flash).

---

## Public surface

### HTTP (portal)

| Method | Path               | Auth  | Body / params     | Purpose                                                                                                                                              |
| ------ | ------------------ | ----- | ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| GET    | `/api/time`        | open  | —                 | `{epoch_us, stratum, synced, last_sync_age_s, upstream, tz, server_running}` — open so the countdown page can also use it as a "device alive" probe. |
| GET    | `/api/time/peers`  | gated | —                 | Detail per upstream: name, last reply ms, offset µs, stratum.                                                                                        |
| POST   | `/api/time/set`    | gated | `{epoch_us}` JSON | Manual set; only honoured if `ntp.accept_set==1` and the device is currently unsynced.                                                               |
| POST   | `/api/time/resync` | gated | —                 | Force an upstream poll now, return new state after a 2 s wait.                                                                                       |
| GET    | `/time`            | open  | —                 | Tasmota-style HTML page: current time, stratum pill, "Sync now" button, table of peers, recent clients.                                              |

`/settings` gets a new collapsible **Time (NTP)** section: enable/server-enable
checkboxes, three upstream inputs, poll-interval, timezone dropdown
(curated list of ~25 common zones + `Custom…`), DHCP-42 toggle, and a
read-only "Last sync: 4 min ago, stratum 3 (time.cloudflare.com)" line.

### MQTT (`$SYS/broker/...`)

| Topic                     | Retained | Payload                              |
| ------------------------- | -------- | ------------------------------------ |
| `$SYS/broker/time`        | no       | epoch seconds, published every 10 s  |
| `$SYS/broker/time/iso`    | no       | `2026-05-13T17:42:09Z`, every 10 s   |
| `$SYS/broker/ntp/synced`  | yes      | `1` / `0`                            |
| `$SYS/broker/ntp/stratum` | yes      | `2`..`16`                            |
| `$SYS/broker/ntp/served`  | yes      | total SNTP responses sent since boot |

### UDP

- `0.0.0.0:123` SNTPv4 unicast only. Multicast/broadcast deliberately
  **off** for v1 — single-NIC ESP-IDF can't filter mDNS-style joins well
  and we don't want to be a noisy neighbour on dorm Wi-Fi.

### DHCP

- The Wi-Fi-AP DHCP server (we already run it) gets **option 42** populated
  with the AP-side IP. Captive clients then auto-discover the NTP server
  on first lease. Implemented through
  `dhcps_set_option_info(42, ...)` after `esp_netif_dhcps_start()` in
  `wifi_connect.c::apply_ap_ip()`.
- We do **not** try to alter the upstream DHCP server's option 42 for STA
  / Ethernet — clients on those networks must point at us manually (or
  via their own router config).

---

## Phasing

### Phase 0 — Test harness (lands first)

Mirrors `plan-mqtt-ux-v2.md` Phase 0.

1. `tests/ntp/test_sntp_client.py` — point a host-side `ntplib` client at
   the device, assert response well-formed and offset < 100 ms when the
   device itself is synced to `pool.ntp.org`.
2. `tests/ntp/test_unsynced.py` — boot the device with WAN blocked
   (firewall rule in the CI runner), query, assert `stratum==16` and
   `LI==3`.
3. `tests/ntp/test_storm.py` — `asyncio` blast 500 req/s for 30 s; assert
   ≥99 % responses, no broker disconnects, heap delta < 4 KB.
4. `tests/api/test_time_endpoints.py` — JSON-schema validation of the four
   new HTTP endpoints, auth-gating matrix.
5. `tools/capture_portal.py` learns `/time` and the new `/settings` panel.

Exit: `make test-ntp` green in CI against host-build firmware.

### Phase 1 — SNTP client (no server yet)

Pure consumer of upstream time. Goal: from this phase forward, every other
component in the broker can call `ntp_now_us()` and get either real epoch
microseconds or `0` (unsynced).

- Add `main/ntp.c` / `main/ntp.h` exposing
  `int64_t ntp_now_us(void)`, `bool ntp_is_synced(void)`,
  `void ntp_get_state(ntp_state_t *)`.
- Wire `esp_sntp_*` to the upstream list, register
  `time_sync_notification_cb` to flip the synced flag and update
  `last_sync_us`.
- Refactor every `esp_timer_get_time()` call **that is rendered to a
  user** (uptime strings, log timestamps, `$SYS/broker/uptime`) to _also_
  emit an absolute timestamp when `ntp_is_synced()`.
- New `$SYS/broker/time` MQTT publishers.
- Portal: `/api/time` (without `server_running`), `/api/time/peers`,
  `/api/time/resync`, settings panel (without DHCP-42 toggle).

Flash budget: ≤ 6 KB (mostly `esp_sntp` already linked by lwip).

Exit: Phase 0 tests #1, #4 green; `$SYS/broker/time` visible in
`mosquitto_sub -t '$SYS/#'`.

### Phase 2 — SNTP server

The "make it useful to clients" phase.

- New FreeRTOS task `ntp_server_task` (prio 5, stack 3072, on PRO core)
  that owns a single UDP socket bound to `0.0.0.0:123`.
- Hot path: `recvfrom` → validate length≥48 and mode in {1,3} → fill
  response buffer → `sendto`. **No `malloc` in the hot path** — one static
  48-byte tx buffer per task.
- Stratum logic:
  - `ntp_is_synced()` false → stratum=16, LI=3, ref_id="INIT" (ASCII),
    ref_ts=0.
  - synced → stratum = clamp(peer_stratum+1, 2, 15), LI=0,
    ref_id = first 4 bytes of upstream IP, ref_ts = last sync.
- Reject packets larger than 68 bytes (NTPv4 control/auth extensions
  we don't support) with silent drop to avoid amplification abuse.
- Rate-limit per source IP to 10 req/s (token bucket, 32-entry LRU). Drops
  beyond that are silent. This caps amplification factor at 1× even if we
  somehow get spoofed.
- Bind path lights up automatically as netifs come up — no per-NIC socket,
  lwip handles that.
- New `$SYS/broker/ntp/served` counter, `/time` portal page.

Flash budget: ≤ 4 KB.

Exit: Phase 0 tests #2, #3 green; `chronyc -h <broker-ip> tracking`
on a Linux box returns sane values within 30 s of broker boot.

### Phase 3 — DHCP option 42 + UX polish

- Populate DHCP option 42 from `apply_ap_ip()`; verify with
  `nmcli -t -f IP4.DOMAIN,IP4.NTP dev show` on a laptop joined to the AP.
- `/time` page gets a 1-Hz live clock (server-side rendered HTML +
  `<meta http-equiv="refresh" content="10">`, no JS required) and a
  recent-clients table (last 16 source IPs, last-seen, request count).
  Source: per-IP token-bucket LRU from Phase 2, no new state.
- Visual diff baselines under `docs/screenshots/ntp/` regenerated via
  `tools/capture_portal.py`.
- README "What's new in 0.7.0" entry.

Flash budget: ≤ 2 KB.

### Phase 4 — Stretch (optional, behind Kconfig)

- `CONFIG_NTP_BROADCAST` — periodic broadcast packets to the AP segment
  every 64 s (RFC 4330 §3.3 broadcast mode), off by default.
- Crude **drift compensation** while free-running: linear-fit the last 8
  upstream offsets, apply correction to `boot_epoch_us` between syncs.
  Should reduce 24 h drift from ~2 s to ~200 ms on ESP32-S3.
- `POST /api/time/set` accepting a manual epoch from the portal as a last
  resort for fully air-gapped installs (already in the NVS schema as
  `accept_set`, just needs the handler + a big red disclaimer).

These move into the plan only if Phase 1–3 ship under the combined
12 KB flash budget.

---

## Risks & mitigations

| Risk                                                                                          | Mitigation                                                                                                                                                                 |
| --------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Amplification / reflection abuse against the WAN.                                             | Server only binds when a netif is UP; AP netif is the typical exposure. Drop oversized packets; rate-limit per source; refuse `mode==6/7` (control/private).               |
| `esp_sntp` ESP-IDF singleton conflicts with future Wi-Fi enterprise auth that also uses SNTP. | Wrap init in `ntp_init()` and guard with `s_sntp_inited` flag; `Kconfig` to disable entirely.                                                                              |
| Clients lock onto bogus time during the few seconds between netif-up and first upstream sync. | Stratum 16 + LI 3 in that window — RFC-compliant clients (chrony, ntpd, w32time, systemd-timesyncd) skip the source. Tested in Phase 0 test #2.                            |
| Reboot loses time → "Last sync" timestamps in the portal go negative.                         | Persist `last_sync_us` to NVS rate-limited (≤4 writes/h); on boot, hold it as "stale" and gray it out in the UI until first new sync replaces it. Never use it as a clock. |
| ESP-IDF v5.5 changed `esp_sntp_*` API vs older `sntp_*`. Need to keep parity.                 | Component sticks to the `esp_sntp_*` symbols only; CI builds against the pinned `dependencies.lock` ESP-IDF version.                                                       |

---

## Acceptance criteria for v0.7.0 release

1. `mosquitto_sub -h <broker> -t '$SYS/broker/time'` prints a Unix
   timestamp within 60 s of cold boot on a WAN-connected network.
2. `chronyc -h <broker> sources -v` from a Linux client on the AP shows
   the broker as `^* <broker-ip>` (a usable source) within 90 s of joining.
3. A laptop joined to the AP receives the broker's IP in DHCP
   option 42 (`nmcli` / `Get-NetIPConfiguration`).
4. Portal `/time` renders in < 100 ms over Ethernet, < 200 ms over Wi-Fi
   STA, identical mobile + desktop captures committed under
   `docs/screenshots/ntp/`.
5. CI: `make test-ntp` + existing `make test-portal` + `make test-api`
   all green; combined flash growth ≤ 12 KB vs 0.6.4.
6. README "What's new in 0.7.0" merged.

---

## Out of scope (v0.7.0)

- NTPv4 authentication (symmetric keys / NTS).
- IPv6.
- PTP / hardware timestamping.
- Acting as a stratum-1 source via attached GPS — would need a UART pin
  pinout that the W5500 SPI bus already contends for.
- Mesh / multi-broker time consensus.

Each of the above is tracked separately and may resurface post-0.8.0.
