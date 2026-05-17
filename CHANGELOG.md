# Changelog

Detailed per-release notes live in [`changelog/`](changelog/).

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
