# mqtt_broker v0.6.0 — UX honesty pass

Built: May 12 2026 17:58:35 · SHA-256: `102cbeac24dd03944564464e505f078fb6fbdcf2af71ded691fbf149e1c4ef21`

OTA-flashed live in an unattended session against the device at
`http://192.168.22.100/`. Screenshots before/after live in
`docs/screenshots/ux-audit/`.

## Defects fixed

| #   | Defect (v0.5.0)                                                                                                                                                                                                             | Fix (v0.6.0)                                                                                                                                                                                                                                                             | Evidence                |
| --- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------- |
| 1   | Dashboard stacked an orange `AP mode — connect to configure WiFi` warning on top of a green `Ethernet — online` pill even when the device had a working uplink. First impression was "needs attention" on a healthy device. | Single coherent pill: `Online · Ethernet <ip>` (or `Online · WiFi <ssid>`, or `Setup · AP mode @ <ip>` only when there is no uplink).                                                                                                                                    | `dashboard_*.png`       |
| 2   | `/information` rendered `AP Password: mqtt1234` in plaintext, GET-able by anyone on the LAN.                                                                                                                                | Renders `set (factory default — change in Configuration)` in orange when default; `set (custom)` in green when changed. Plaintext never appears in the HTML.                                                                                                             | `information_*.png`     |
| 3   | `/settings` form pre-filled `<input value='mqtt1234'>` for the AP password (and the MQTT auth password) — masked dots in the UI but plaintext in View Source.                                                               | Password inputs render empty with placeholder `unchanged — leave blank to keep`. Save handler treats an empty submission as "keep current value". Explicit clear path documented (`auth_pass_clear=1`).                                                                  | `settings_*.png`        |
| 4   | `/settings` AP password input was `required`, so saving NAPT or hostname forced you to re-type the AP password every time.                                                                                                  | `required` attribute removed; min-length 8 still enforced when the user does change it.                                                                                                                                                                                  | `settings_*.png`        |
| 5   | `/tester` filter input was collapsed to a ~5 px sliver — the page-level `.btn { width: 100% }` was pinning the Pause/Clear buttons to full width inside a flex row, eating all the input's space.                           | Tester rows now use `flex-wrap: wrap`; buttons scoped to `width: auto`; filter input has `min-width: 120px`. Mobile reflows cleanly.                                                                                                                                     | `tester_*.png`          |
| 6   | `/tester` payload `maxlength=256` was too small for real-world JSON payloads.                                                                                                                                               | Bumped to 1024 (UI + broker). Added a live `N/1024` byte counter next to the Payload label. Bumping further requires growing the stream-buffer slots; 1024 stays within RAM budget.                                                                                      | `tester_*.png`          |
| 7   | `/tester` Filter placeholder advertised `+ and # support` but the code stripped those characters and did substring match.                                                                                                   | Implements real MQTT 3.1.1 §4.7 topic-filter matching client-side. `+` matches one level, `#` matches the remainder. Falls back to substring when input has no `/`/`+`/`#`.                                                                                              | `tester_*.png`          |
| 8   | `/tester` had only a `retain` checkbox; no QoS selector despite the broker supporting QoS 0/1.                                                                                                                              | Added QoS dropdown (0/1) next to retain. WS message now carries `qos`; broker tester path currently treats both as 0 (next step: thread QoS through `broker_tester_request_publish`).                                                                                    | `tester_*.png`          |
| 9   | `/clients` did `setTimeout(location.reload, 5000)` — full page reload every 5 s. Destroyed scroll, selection, mid-copy state; re-rendered ~4 KB of HTML every cycle.                                                        | Replaced with in-place fetch+patch of `/api/clients` every 3 s. Pauses when tab is hidden. Visible `Live · last update HH:MM:SS` indicator plus a `pause` button. `<noscript>` fallback still does a hard reload.                                                        | `clients_*.png`         |
| 10  | `/update` showed `Running Partition: ota_1` but offered no way to switch back if the new image misbehaved.                                                                                                                  | New "Other Partition" row shows the inactive slot's version/project. When that slot holds a valid app, a Rollback panel appears with a `Roll back & Reboot` button (`POST /ota-rollback`). Calls `esp_ota_set_boot_partition` on the other slot and reboots. Idempotent. | `firmware_update_*.png` |
| 11  | `/ota-url` rejected `https://` URLs via an HTML5 `pattern='http://.*'` and a strict server check.                                                                                                                           | Both schemes accepted on client and server. Label updated to `(http:// or https://)`.                                                                                                                                                                                    | `firmware_update_*.png` |

## Files changed

```
main/portal.c            +315  -50    (status pill, password hide, tester fixes,
                                       clients polling, rollback panel/handler,
                                       https in ota-url)
main/mqtt_broker.h        +5   -1    (BROKER_TESTER_MAX_PAYLOAD_LEN 256 → 1024)
main/version.h            +2   -2    (0.5.0 → 0.6.0)
tools/capture_portal.py  new          (Playwright capture script)
plan-mqtt-ux-v2.md       new          (UX audit + phased plan)
docs/screenshots/        refreshed    (legacy flat names from live device)
docs/screenshots/ux-audit/ new        (14 captures, desktop + mobile)
```

Binary size: **1.07 MB → 1.12 MB** (+50 KB). 73 % of the 4 MB OTA slot still
free.

## Not done in this pass (deliberately)

These need their own release because they have lock-out / brick risk an
unattended session cannot recover from:

- **CSRF tokens on POST endpoints.** Doable but if I mis-implement and the
  rollback POST breaks, I can't fix it without serial. Goes in next release
  alongside a guaranteed-good escape hatch.
- **Basic-Auth-on-by-default option.** Same reason — easy to lock yourself out.
- **Bootloader-level auto-rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
  Requires an in-app self-test that calls
  `esp_ota_mark_app_valid_cancel_rollback` within a deadline. Adding it
  without the self-test wired up would brick all upgrades.
- **QoS 1 in `/tester` publish.** UI sends `qos` already; broker tester path
  needs to thread it through `broker_tester_request_publish`.
- **Retained-message browser** (`/retained`, `GET/DELETE /api/retained`).
- **Server-side topic filter on `/ws`** (`?sub=home/#`) so the broker doesn't
  fan out the full firehose to every WS consumer.
- **`/config` SSID scan + signal bars + "currently configured" indicator.**
- **`/reboot` countdown page** that polls `/api/status` until it 200s.

All listed in `plan-mqtt-ux-v2.md` with phases and sizing.

## How v0.6.0 was deployed

```
PORTAL=http://192.168.22.100
curl -s -F "firmware=@build/mqtt_broker.bin" $PORTAL/ota-upload   # 20s
# device reboots, comes back in ~5s on ota_0
curl -s $PORTAL/api/status | jq .firmware.version                  # "0.6.0"
PORTAL_URL=$PORTAL python3 tools/capture_portal.py                 # re-capture
```

If anything had gone wrong, recovery path was either:

- Rollback button on `/update` (now in firmware — but not in v0.5.0!), or
- POST `releases/rollback.bin` (≈ v0.4.x) at `/ota-url` from a local HTTP
  server, or
- Serial reflash via `idf.py flash`.
