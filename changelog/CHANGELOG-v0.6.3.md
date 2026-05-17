# mqtt_broker v0.6.3 — Save = confirm + reboot + countdown

Built: May 13 2026 10:35 · SHA-256:
`9f04ef8543ec8654b592abecb6fcc5570e9b383bc0262518dfa471abcf42d222`

## What changed

### 1. `/settings` and `/config` now save-and-reboot

Previously, the Configuration form silently 302'd back to `/settings` after
NVS writes. Several settings (MQTT port, buffer size, retained-message
toggle, AP password, AP IP, hostname) only actually took effect on the next
reboot, but the UI gave no indication of that — users would change the
"Enable retained messages" checkbox, save, then be confused that retained
messages weren't being honored until they noticed and clicked Restart.

Now:

1. **Form ends with a green `Save & Reboot` button** plus a one-line note:
   *"Saving will reboot the device so all changes take effect uniformly.
   Reconnect in about 10 seconds."* — same copy on `/settings` and
   `/config`.
2. **Browser `confirm()`** runs *before* the POST is dispatched. Cancel
   means cancel: nothing is written. Wording is explicit ("Save settings
   and reboot? The device will restart and should be reachable again in
   about 10 seconds.").
3. **`POST /save-settings`** persists every field, logs
   `Settings saved; rebooting on user request`, serves the reboot-
   countdown page (3142 bytes — same template as `GET /reboot` and
   `POST /ota-rollback`, tailored title `Saving and rebooting`),
   `close(fd)`, `vTaskDelay(500ms)`, `esp_restart()`.
4. **`POST /save`** does the same for WiFi credentials. Subtitle includes
   the saved SSID so the user can verify what they're about to associate
   with: `Saved credentials for <SSID>. Reconnecting...`
5. **Countdown JS** is unchanged from 0.6.2: polls `/api/status` every
   1 s, gates "back online" on the offline edge, auto-redirects 800 ms
   after detection. `<noscript>` meta-refresh fallback still works.

The NAPT toggle previously took effect immediately (calling
`eth_napt_enable()` / `eth_napt_disable()` in the handler). With the
unified "save = reboot" model that's redundant — the reboot re-applies
NAPT from NVS cleanly. Code reduced to NVS-only; comment updated.

### 2. `CMAKE_CONFIGURE_DEPENDS` on `main/version.h`

The 0.6.2 PROJECT_VER fix relied on `file(READ ...)` at CMake configure
time. CMake doesn't re-run configure when only the read-once file changes,
so bumping `FW_VERSION` silently re-used the cached `PROJECT_VER`. Fixed
with:

```cmake
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/main/version.h")
```

Confirmed working: editing `version.h` from 0.6.2 → 0.6.3 and running
`idf.py build` now prints
`-- mqtt_broker: PROJECT_VER=0.6.3 (from main/version.h)` and embeds
0.6.3 in the image header without a manual `idf.py reconfigure` step.

## End-to-end verification

```
=== Pre-save state ===
  v=0.6.3 up=96s retain=15 retain_kb=5

=== Save & Reboot via POST (retain TTL 4h -> 24h) ===
  HTTP:200 bytes:3142
  Saving and rebooting       <- title
  Waiting for the device     <- initial msg
  api/status',{cache:'no-store  <- poll embedded

=== Waiting for reboot ===
  Back at +47s, uptime=41s

=== Post-save state ===
  v=0.6.3 up=41s clients=6

=== Verify retain TTL persisted as 24h ===
  24 hours                   <- from /information
```

Saved retain TTL survived the reboot, all 6 Tasmota clients reconnected,
device on 0.6.3.

## Files changed

```
main/portal.c            +49  -34   (Save & Reboot button copy, /save-settings
                                      and /save handlers swap redirect for
                                      countdown+restart, NAPT immediate-apply
                                      block trimmed since reboot covers it)
main/version.h           +2   -2    (0.6.2 -> 0.6.3)
CMakeLists.txt           +5   -0    (CMAKE_CONFIGURE_DEPENDS on version.h)
tools/capture_save_reboot.py  new   (overlay captures of confirm prompt
                                      and the renamed countdown title)
docs/screenshots/ux-audit/save_reboot_confirm.png    new
docs/screenshots/ux-audit/save_reboot_countdown.png  new
README.md                +5   -1   (What's new section, version table)
CHANGELOG-v0.6.3.md      new
```

Binary size: 1.13 MB → 1.13 MB (+0.5 KB net, the new copy is offset by the
removed `eth_napt_enable`/`disable` immediate-apply code).
