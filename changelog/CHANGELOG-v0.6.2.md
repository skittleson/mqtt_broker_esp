# mqtt_broker v0.6.2 — Reboot countdown + honest version display

Built: May 13 2026 08:38:41 · SHA-256: `00f868f2338c0e32ae2f64eccae17169dab68d1f79918483d849225db77b8aee`

OTA-flashed live against the device at `http://192.168.22.100/`. Screenshots
of the new flow live in `docs/screenshots/ux-audit/rebooting_*.png` and
`docs/screenshots/ux-audit/reboot_real_offline.png` (real device reboot).

## What changed

### 1. Reboot countdown page

Whenever the device intentionally goes away, the user used to land on a
broken socket with a `Rebooting...` plain page and no signal for when it
was safe to retry. Now:

- New helper `http_send_reboot_countdown(fd, title, subtitle, return_path)`
  emits a standalone HTML+JS page that:
  - polls `GET /api/status` every 1 s with `cache: 'no-store'` and a
    1.5 s `AbortController` timeout per request,
  - tracks the **offline edge** so a pre-reboot in-flight response can't
    falsely claim "back online",
  - cross-checks **uptime regression** (new `uptime_s` smaller than the
    baseline) as a backup signal for very fast reboots,
  - shows a pulsing orange dot during the wait and a green dot the moment
    the device returns,
  - auto-navigates to `return_path` 800 ms after detecting back-online
    (gives the user time to see the green pill),
  - falls back to `<noscript><meta http-equiv='refresh' content='15'>`
    for JS-disabled clients,
  - shows live elapsed-seconds and a progress bar (capped at 90 % until
    confirmed back online).

Wired into:
- `GET /reboot` (manual reboot)
- `POST /ota-rollback` (rollback button on `/update`)
- `handle_ota_url` success path (OTA via URL)
- New read-only `GET /rebooting` endpoint (does NOT reboot) — used by
  the `/update` upload XHR to redirect users straight to the countdown
  instead of waiting on a frozen 10-second progress bar.

### 2. Honest version display on `/update` and Rollback panel

The OTA image header's `esp_app_desc_t.version` field used to default to
`git describe` output (e.g. `tester-v0.3.0-4-g30c64db-dirty`), which made
the `/update` page's "Other Partition" row and Rollback panel misleading:
the user couldn't tell what version they'd be reverting to.

Fix: `CMakeLists.txt` now reads `FW_VERSION` out of `main/version.h` at
configure time and sets `PROJECT_VER` before `include(project.cmake)`.
The build log prints `PROJECT_VER=0.6.2 (from main/version.h)` so it's
obvious what got embedded. After two OTA cycles to populate both slots,
the `/update` page now shows:

  Other Partition:  ota_0 — 0.6.2
  Rollback text:    Switch the boot partition to ota_0 (mqtt_broker 0.6.2)
                    and reboot.

Single source of truth: bump `FW_VERSION` in `main/version.h`, both the
portal display strings AND the image header pick it up.

## Files changed

```
main/portal.c          +120  -53    (countdown helper, /rebooting endpoint,
                                     /update XHR redirect, three callsite
                                     conversions to the new helper)
main/version.h         +2   -2     (0.6.1 → 0.6.2)
CMakeLists.txt         +12  -0     (PROJECT_VER from main/version.h)
tools/capture_reboot.py new        (Playwright capture for the 3 UI states,
                                     uses page.route to mock /api/status
                                     so no actual reboot needed)
docs/screenshots/ux-audit/rebooting_offline.png    new
docs/screenshots/ux-audit/rebooting_backonline.png new
docs/screenshots/ux-audit/reboot_real_offline.png  new (from a real reboot)
README.md              +6   -2     (What's new section, endpoint table)
```

Binary size: 1.12 MB → 1.13 MB (+2 KB). Still 73 % of the 4 MB OTA slot free.

## Real-reboot timing observed

```
Pre-reboot uptime:        372 s
GET /reboot returns:      HTTP 200, 3104 bytes  (the countdown page)
Concurrent /rebooting:    HTTP 000 (refused — device is mid-reboot)
First successful poll:    +27 s from /reboot trigger
Fresh uptime at recovery: 20 s
```

For a 1.1 MB OTA upload over Ethernet the typical cycle is ~25–35 s end to
end (upload 20 s + reboot 5–10 s). The countdown page covers that span
cleanly with visible elapsed-seconds feedback.
