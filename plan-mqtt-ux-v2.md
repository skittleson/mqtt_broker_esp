# Portal UX Improvement Plan v2

Target: `http://192.168.22.100/` (mqtt_broker 0.5.0).
Goal: keep the Tasmota soul (dark, compact, no-JS-fallback friendly) but fix real
defects and make the UI testable end-to-end.

All findings below are backed by Playwright captures in
`docs/screenshots/ux-audit/` (desktop 1024 × 900, mobile 390 × 844 @ 2x).
Regenerate any time with:

```bash
PORTAL_URL=http://192.168.22.100 python3 tools/capture_portal.py
```

## Visual evidence of current defects

| Finding                                                                                                                                                                                               | Evidence                           |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| Contradictory status pills on home (orange AP-warn stacked on green Ethernet-OK)                                                                                                                      | `ux-audit/dashboard_desktop.png`   |
| `/information` prints `AP Password: mqtt1234` in plaintext                                                                                                                                            | `ux-audit/information_desktop.png` |
| `/settings` ships AP password back as `value='mqtt1234'` (visible in View Source) and the retained-messages checkbox state disagrees with `/information`                                              | `ux-audit/settings_mobile.png`     |
| `/tester` filter input is collapsed to a ~5 px sliver between label and the full-width Pause/Clear buttons; log pane stayed empty for 1.2 s despite 6 active publishers                               | `ux-audit/tester_desktop.png`      |
| `/clients` mobile: 8-column table wraps `0h 11m 35s` and `4s ago` onto two lines per row; tiny disclaimer `Page auto-refreshes every 5 seconds` is the only hint that the full page is being reloaded | `ux-audit/clients_mobile.png`      |
| `/config` (WiFi) has no SSID scan, no signal bars, no "currently configured" indicator — type blind                                                                                                   | `ux-audit/wifi_config_desktop.png` |

Guiding rules

- Every change ships with a test (Playwright for UI, pytest for API).
- No new feature without an `/api/*` JSON endpoint behind it (UI + headless parity).
- No regression in flash size > 8 KB per phase; budget tracked in CI.
- Mobile first: usable on a 360 px viewport, single-thumb reach.

---

## Phase 0 — Test harness (lands first, blocks nothing else)

Without this, every later phase is guesswork.

1. `tests/portal/` — Playwright (Python, already in repo) running against
   `PORTAL_URL` env (defaults to `http://192.168.22.100`).
   - Smoke per page: 200, has nav, no JS console errors.
   - Visual diff baseline against `docs/screenshots/ux-audit/` (one image
     per page, mobile 390 px + desktop 1024 px). `tools/capture_portal.py`
     is the canonical generator — CI re-runs it and `git diff --stat`
     under `docs/screenshots/` must be empty on green builds.
   - Form round-trip: POST a setting, GET, assert echo.
2. `tests/api/` — pytest hitting `/api/status`, new `/api/clients`,
   `/api/retained`, `/api/settings` (GET). Schema-validated with `jsonschema`.
3. `tools/lighthouse.sh` — Lighthouse CI in mobile mode, fail if a11y < 90 or
   perf < 80.
4. Fuzz: `tests/fuzz/test_forms.py` — Hypothesis-driven POSTs to `/save`,
   `/save-settings` checking the broker never wedges.
5. GitHub Actions: build firmware, flash to a QEMU ESP32 (or skip and run
   tests against a host-side stub that serves the same HTML — `portal_host.c`
   compiled for Linux).

Exit criteria: `make test-portal` green in CI against a host build.

---

## Phase 1 — Honesty pass (no new features, just stop lying)

Pure correctness fixes. Small diffs, big trust gains.

1. **Status banner logic** (`portal.c` home handler):
   - Single pill, computed: `Online · Ethernet 192.168.22.100` (green)
     OR `Online · WiFi <ssid> <rssi>` OR `Setup · AP mqtt-broker @ 192.168.25.1`.
   - Show AP-mode pill _only_ when no STA + no Ethernet IP.
2. **Stop leaking the AP password** on `/information` and `/settings`.
   - `/information`: display `••••••••` with a `Reveal` button that calls
     `/api/ap-pass` (auth-gated, see Phase 4).
   - `/settings`: render the field empty with placeholder
     `unchanged — leave blank to keep`. Same for MQTT auth password.
3. **Tester filter honesty**: rename label to `Topic substring` and remove the
   `+ # supported` claim, OR implement real wildcard matching client-side
   (cheap: ~20 lines of JS, do this one).
4. **Tester payload limit**: bump `maxlength` to 4096, show byte counter.
5. **/ota-url**: accept `https://` (ESP-IDF supports it; just remove the
   `pattern='http://.*'` and document cert handling — for now, plain TCP with
   warning is fine, but stop forbidding `https`).
6. **Tester wildcard claim** in subscribe filter: implement server-side topic
   filter (`?filter=home/#`) on `/ws` so the broker doesn't fan out everything.

Tests: every fix gets a Playwright assertion + an API check.

---

## Phase 2 — Information architecture

Tasmota-style buttons stay, but add scaffolding.

1. **Persistent top bar** — 32 px high, contains:
   - Device name (clickable → `/`)
   - Live status dot (green/yellow/red, driven by `/api/status` polled every
     5 s via fetch, _not_ page reload)
   - Uptime ticker
   - Settings cog → `/settings`
2. **Replace /clients full-page reload with `fetch('/api/clients')` + DOM
   patch** every 3 s. Pause when tab hidden (`visibilitychange`).
3. **Breadcrumb** under the bar: `MQTT Broker › Clients`. One source of truth.
4. **Split /settings into tabs** (CSS-only, anchor-targeted, no JS required):
   `Device | MQTT | Retained | Network | Access Point`. Each tab posts its own
   subset; saving NAPT no longer requires retyping the AP password.
5. **404 page** with a search/links list (we don't have one).
6. **Toast on save**: after redirect from `/save-settings`, show a 3 s pill
   `Saved 14:32:08` driven by a `?saved=1&at=...` query param. Pure server
   render, works without JS.

---

## Phase 3 — New capability: retained-message browser

Biggest gap vs Tasmota's MQTT console. Device reports 15 retained / 5 KB and
gives you no way to see or delete them.

1. New `GET /api/retained` → array of `{topic, size, qos, age_s}` (no payload
   in list view to keep heap calm).
2. `GET /api/retained?topic=foo/bar` → full message, base64 if non-UTF8,
   `Content-Type: application/octet-stream` if `?raw=1`.
3. `DELETE /api/retained?topic=...` → publishes a zero-byte retained message
   per MQTT 3.1.1 §3.3.1.3 (correct way to clear).
4. New page `/retained`: paginated table (50/page), filter input, click a row
   to expand payload (decoded as UTF-8 if printable, else hexdump). Per-row
   `Delete` button with confirm.
5. Bulk: `Delete all matching <filter>` button (red, double-confirm).
6. Memory guard: if heap < 32 KB, API returns 503 with `Retry-After: 5`.

Tests: pytest creates 100 retained msgs over real MQTT, asserts list, delete,
and bulk delete.

---

## Phase 4 — Security baseline

Not optional for "useable by everyone". This is a LAN device but the LAN is
not trusted (IoT subnet has random vendor crap on it).

1. **CSRF token**: 16-byte random in NVS at boot, embedded as hidden input on
   every form and required in `X-CSRF` header for JSON POSTs. Reject mismatch
   with 403.
2. **Optional Basic Auth for portal** (separate from MQTT auth): toggle in
   Settings → Device. Default off (keeps zero-config story), but one click on.
3. Rate-limit `/save`, `/save-settings`, `/ota-*`, `/reboot` to 5/min per IP
   (token bucket in `portal.c`, 32 buckets in PSRAM).
4. **Disable directory listing / unknown-path leakage** — currently unknown
   paths probably 404 OK, but verify no stack traces.
5. AP-password reveal endpoint (Phase 1.2) requires CSRF + (if portal auth
   on) Basic Auth.
6. `Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'`
   header — we inline CSS but never load remote anything.

Tests: pytest CSRF replay/missing/mismatch matrix; Lighthouse a11y still ≥ 90.

---

## Phase 5 — OTA done right

1. **Rollback button** on `/update` when `running_partition == ota_1` and
   `ota_0` has a valid app (and vice versa). Calls `esp_ota_set_boot_partition`
   on the _other_ slot and reboots.
2. **Pre-flight check** before flashing: HEAD the URL, refuse if Content-Length
   > free OTA partition size, show expected vs available.
3. **Optional SHA-256 field** on URL form; verify during stream-in, abort on
   mismatch _before_ `esp_ota_end`.
4. **Post-OTA self-test**: new image must POST `/api/ota-confirm` within 60 s
   of boot or we mark it invalid and roll back automatically (uses
   `esp_ota_mark_app_valid_cancel_rollback`).
5. **Reboot countdown page** after `/reboot`: server sends an HTML page that
   polls `/api/status` every 1 s, switches to a green "Back online" link when
   it 200s. Replaces the current broken-page experience.
6. GitHub Releases integration (optional, behind a checkbox): `/api/releases`
   proxies `https://api.github.com/repos/<owner>/<repo>/releases/latest` and
   `/update` shows `New: 0.6.0 — Install` if newer than `version.h`.

---

## Phase 6 — Tester upgrade

Make it the actual `mosquitto_sub` / `pub` replacement people reach for.

1. **Publish**: QoS 0/1 selector, payload up to 4 KB, byte counter,
   "send hex" toggle for binary, last-10 publish history (localStorage).
2. **Subscribe**: real MQTT filter sent server-side (`/ws?sub=home/#`).
   Multiple filters with chips. Auto-reconnect with visible backoff timer.
3. **Per-message actions**: copy topic, copy payload, "publish back" prefill,
   "delete retained" (if `r=1`).
4. **Pause/resume preserves buffer**, max 1000 msgs ring buffer, "Export JSONL".
5. Connection pill becomes a tri-state: `connected | reconnecting (3 s) | offline`.
6. Keyboard: `/` focuses filter, `p` toggles pause, `c` clears (a11y: with
   visible help line).

---

## Phase 7 — Accessibility & mobile polish

1. All inputs get `<label for>`; all buttons get `aria-label` where icon-only.
2. Color contrast: `#aaa` on `#252525` fails WCAG AA. Bump to `#cfcfcf`.
3. `/clients` table → responsive: collapse to cards under 600 px.
4. Focus rings restored (`:focus-visible` outline 2 px `#1fa3ec`).
5. `prefers-reduced-motion` honored on the connection pill blink.
6. Optional light theme via `prefers-color-scheme` (CSS variables refactor —
   one-time cost, ~200 B gz).
7. Lighthouse a11y ≥ 95 enforced in CI.

---

## Sequencing & rough sizing

| Phase | Effort (dev-days) | Flash Δ | Risk                | Ship gate      |
| ----: | ----------------: | ------: | ------------------- | -------------- |
|     0 |                 2 |       0 | low                 | CI green       |
|     1 |                 1 |      ~0 | low                 | Phase 0 done   |
|     2 |                 3 |   +3 KB | med                 | Phase 0 done   |
|     3 |                 2 |   +4 KB | med                 | Phase 0+1      |
|     4 |                 2 |   +2 KB | high (lockout risk) | Phase 1+2      |
|     5 |                 3 |   +3 KB | high (brick risk)   | Phase 4 (CSRF) |
|     6 |                 2 |   +2 KB | low                 | Phase 2        |
|     7 |                 1 |   +1 KB | low                 | any time       |

Total: ~16 dev-days, ~15 KB flash. Current image has plenty of headroom in
the 4 MB OTA slot.

---

## What I'd cut if forced

If you only have a weekend: **Phase 0 + Phase 1 + Phase 4 (CSRF only) +
Phase 5.1 (rollback) + Phase 2.2 (no-reload /clients)**. That alone removes
every actual defect I found and adds the one feature (rollback) that has
saved my bacon on every long-running ESP32 deployment.
