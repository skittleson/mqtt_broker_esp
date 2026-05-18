# Changelog

Detailed per-release notes live in [`changelog/`](changelog/).

## 0.8.3 — Author attribution in portal footer

Footer on every portal page and the Information page's Firmware row now
reads `mqtt_broker 0.8.3 by Spencer Kittleson`. New `FW_AUTHOR` and
`FW_FOOTER` macros in `version.h` keep the two render sites in sync
automatically across future version bumps. +48 bytes binary.
Details: [`changelog/CHANGELOG-v0.8.3.md`](changelog/CHANGELOG-v0.8.3.md).

## 0.8.2 — Timezone preset dropdown + /timers polish + JSON write API

Headline: **the Timezone field on `/settings` is now a dropdown of ~40
IANA-derived presets**, so picking 'US — Pacific (PT, DST)' writes the
full `PST8PDT,M3.2.0,M11.1.0` POSIX string for you. The text input remains
the source of truth — exotic zones still hand-type. Closes the 10-year-
lifetime story: DST rules live in user-editable NVS, not code.

Also ships the rest of the /timers UX audit follow-ups: mobile **card
layout** below 600 px (no JS, pure CSS media query); master pause is now
an inline pill in the header instead of a banner-style button; Save /
Test fire / Clear share a flex row on desktop via HTML5 form= association.

Write half of the JSON API lands: `PUT /api/timers/<n>` (validates,
returns next_fire_unix) and `DELETE /api/timers/<n>`. Both CSRF-protected.
+5 KB binary. Details: [`changelog/CHANGELOG-v0.8.2.md`](changelog/CHANGELOG-v0.8.2.md).

## 0.8.1 — /timers UX fixes

Close every correctness defect from the 0.8.0 UX audit plus four higher-impact
polish items. Numeric TZ offset on `/timers` (no more lying "UTC" label on a
PDT device); dropped the "24h" claim on the time input (browsers honour
locale); empty slot's edit form no longer pre-fills a misleading `12:00 AM`.
Dedicated `Rep` column with ↻ / 1× icons; three-state On indicator (● armed
/ ◐ disarmed / — empty); **`Next fire` line under the time picker** so users
can sanity-check their schedule against the device's TZ in-context; trimmed
footer noise. No protocol or storage changes. +1.2 KB binary.
Details: [`changelog/CHANGELOG-v0.8.1.md`](changelog/CHANGELOG-v0.8.1.md).

## 0.8.0 — Scheduled MQTT publishes

Tasmota-style timer scheduling. 16 wall-clock slots fire MQTT publishes at
configured local times on selected days of the week. New `/timers` portal
page, `/api/timers` JSON, and `broker_publish_local()` API for non-client
publishes. DST handled automatically via the POSIX TZ string in NVS —
user-editable so the device stays correct across rule changes for 10+ years
without reflash. Per-slot QoS 0/1, retain, ±0–15 min window jitter, repeat
or one-shot. Master pause toggle. Test-fire button per slot.
Details: [`changelog/CHANGELOG-v0.8.0.md`](changelog/CHANGELOG-v0.8.0.md).

## 0.7.0 — NTP support

LAN-local time source. Built-in SNTP client (`esp_sntp`, up to 3 upstreams) and
SNTPv4 server on UDP :123 with anti-amplification and per-source rate limiting.
mDNS `_ntp._udp` advertisement so Avahi-aware clients auto-discover the broker
as a time source. New `/time` portal page, `/api/time`, `/api/time/resync`,
and `$SYS/broker/{time,ntp/*}` topics. Measured drift from `pool.ntp.org`
well below ±50 ms. `make test` (129 assertions) green on live hardware.
Details: [`changelog/CHANGELOG-v0.7.0.md`](changelog/CHANGELOG-v0.7.0.md).

## 0.6.6 — portal latency

`portal_http`, `portal_dns`, and per-WS tasks pinned to CPU 0. HTTP listen
backlog 4 → 8. Per-request access log. **p95 latency 129 ms → ~54 ms (-58%)**;
requests > 100 ms went from 12% → 0%.
Details: [`docs/portal-latency-analysis.md`](docs/portal-latency-analysis.md),
[`changelog/CHANGELOG-v0.6.6.md`](changelog/CHANGELOG-v0.6.6.md).

## 0.6.4 — auth-aware reboot countdown

New open `/api/ping` (uptime-only) replaces `/api/status` as the countdown poll
endpoint, so Basic Auth no longer triggers a native auth dialog mid-reboot.
Countdown treats any HTTP response as "alive". Auto-redirect 800 ms → 400 ms.
Details: [`changelog/CHANGELOG-v0.6.4.md`](changelog/CHANGELOG-v0.6.4.md).

## 0.6.3 — save = confirm + reboot

`/settings` and `/config` end with a green **Save & Reboot** button + native
`confirm()`. `CMAKE_CONFIGURE_DEPENDS` on `main/version.h` so OTA image
headers can't cache stale versions.
Details: [`changelog/CHANGELOG-v0.6.3.md`](changelog/CHANGELOG-v0.6.3.md).

## 0.6.2 — reboot countdown page

Replaces the broken `Rebooting...` dead-end. Polls `/api/status` every 1 s,
watches for the offline edge, swaps itself for a green `Back online` link when
the new firmware answers. `esp_app_desc_t.version` now matches `FW_VERSION`,
so the `/update` rollback display is honest.
Details: [`changelog/CHANGELOG-v0.6.2.md`](changelog/CHANGELOG-v0.6.2.md).

## 0.6.0 — UX honesty pass

Single coherent dashboard status pill. No more password leaks in `/information`
or `/settings`. Live `/clients` without page reloads. Firmware rollback button.
Real MQTT 3.1.1 §4.7 wildcard matching in the tester. `/ota-url` accepts
`https://`.
Details: [`changelog/CHANGELOG-v0.6.0.md`](changelog/CHANGELOG-v0.6.0.md).
