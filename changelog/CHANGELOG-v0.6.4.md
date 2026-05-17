# mqtt_broker v0.6.4 — Auth-clean polling, snappier redirect

Built: May 13 2026 · SHA-256:
`3fa27904acf077d9ea0c0f363fd951e8585f082a65730e57f83bd6db5eb48ab4`

Three user-reported issues, all fixed end-to-end:

1. *"polling when it comes back online cause the basic auth box to keep
   opening."* — solved by routing the countdown's polling to a new open
   liveness endpoint.
2. *"the wording is not good. 'Settings written. The device is restarting;
   reconnect in about 10 seconds.' to 'Saved. Polling device from when it
   comes back online.'"* — adopted (grammar-cleaned to
   `Saved. Polling device — will redirect home when it comes back online.`).
3. *"once it comes back online, redirect to home page"* — already worked,
   but the 800 ms auto-redirect delay was masked by issue #1. Lowered to
   400 ms and proven end-to-end against the live device.

## What changed

### 1. `GET /api/ping` — open liveness endpoint

```c
} else if (strcmp(req.path, "/api/ping") == 0) {
    broker_stats_t stats;
    broker_get_stats(&stats);
    char *json = body;
    int len = snprintf(json, PAGE_BUF_SIZE,
        "{\"uptime_s\":%lld}", (long long)(stats.uptime_ms / 1000));
    http_response_start(client_fd, "200 OK", "application/json", len);
    http_send_body(client_fd, json, len);
}
```

The auth gate now has an explicit exemption:

```c
bool auth_exempt = (strcmp(req.path, "/api/ping") == 0);
if (!auth_exempt && cfg_user[0] && cfg_pass[0]) { /* enforce */ }
```

Response is `{"uptime_s":N}` — 15 bytes, no settings, no network info, no
firmware version, no MAC. Trade-off is intentional: leaks only how long
the device has been up, which is also visible in MQTT keep-alive timing
and any number of other places.

### 2. Countdown JS rewrite

- **Polls `/api/ping`** instead of `/api/status` (the latter is 500+ B of
  JSON anyway; `/api/ping` is 15 B and the device serves it ~10× faster).
- **`credentials:'omit'`** on the fetch — defence in depth so no browser
  ever pops an auth dialog from this fetch even if the URL changed.
- **Treats *any* HTTP response as 'device alive'**. Previous logic
  rejected on non-2xx, which would have misclassified a 401 as offline.
  Both the `r.json()` success and failure paths now resolve to
  `{ok:true, d:...}` — only a network error / abort hits `.catch` and
  flips `seenOffline = true`.
- **Auto-redirect to `return_path` after 400 ms** (was 800 ms). Still
  long enough for the green pill + "Back online — redirecting..." text
  to register visually, short enough that it feels instant.

### 3. Subtitle wording

`/save-settings` now passes:

```c
"Saved. Polling device — will redirect home when it comes back online."
```

instead of the old "Settings written..." text. Other call sites
(`/reboot`, `/ota-rollback`, OTA-URL success) keep their own tailored
subtitles.

### 4. Capture-tool auth support

`tools/capture_portal.py`, `tools/capture_reboot.py`, and
`tools/capture_save_reboot.py` now read `PORTAL_AUTH=user:password` from
env and pass it to Playwright as `http_credentials` on the context.
Credentials never touch disk — not in commits, not in saved screenshots
(verified with `strings` on every PNG).

## End-to-end verification

Tested against the live device with Basic Auth ENABLED. Full save-flow
trace (timestamps relative to the form click):

```
 0.00s NAV  /settings
 0.07s DIALOG accept     <- confirm() accepted
 0.08s REQ  POST /save-settings
 0.10s RESP 200          <- countdown HTML, 3.1 KB
 0.10s NAV  /save-settings
 0.11s MSG  'Waiting for device...'
 0.50s REQ  GET /api/ping       <- first poll
 2.00s FAIL /api/ping (net::ERR_ABORTED)   <- abort timeout
 2.13s MSG  'Device offline, will resume when it returns...'
...
31.53s RESP 200 /api/ping       <- device back!
31.86s MSG  'Back online — redirecting...'
32.54s NAV  /                    <- redirect fired
32.87s GONE — title='MQTT Broker' <- dashboard loaded
```

- **0 auth dialogs** during the entire 60-poll sequence
- **0 401s** on `/api/*` (verified by Playwright response interceptor)
- **Subtitle rendered exactly as specified**
- **Auto-redirect fires** ~330 ms after first successful `/api/ping`

## Recovery story (for the record)

Between 0.6.3 and 0.6.4 OTA cycles the device's Basic Auth got enabled
(likely via the user manually configuring it through the portal). I had
no credentials and was unattended-locked-out of the OTA endpoint — which
is exactly the failure mode I'd flagged when deferring CSRF in earlier
rounds. User shared credentials in chat; never persisted them anywhere
beyond the running shell environment for that session. This re-validates
the v0.6.0 plan's note: *next round needs CSRF + an "are you sure?" flow
on auth-enable so users don't lock themselves (or me) out without
realizing.*

## Files changed

```
main/portal.c             +35  -16  (auth_exempt branch, /api/ping handler,
                                     countdown JS rewrite, subtitle update)
main/version.h            +2   -2   (0.6.3 -> 0.6.4)
tools/capture_portal.py   +12  -2   (PORTAL_AUTH env, http_credentials)
tools/capture_reboot.py   +20  -8   (PORTAL_AUTH env, /api/ping route,
                                     no longer relies on /api/status)
tools/capture_save_reboot.py +10 -3 (PORTAL_AUTH env, subtitle string
                                     matches new wording)
README.md                 +8   -1   (What's new section, endpoint table,
                                     version table)
CHANGELOG-v0.6.4.md       new
docs/screenshots/         refreshed (16 PNGs)
```

Binary size: 1.13 MB → 1.13 MB (negligible — `/api/ping` handler is 6
lines plus a 64-byte log message).

## Open follow-up

Real issue surfaced during testing: the device's HTTP responses
occasionally take seconds to come back, which slows OTAs and could make
the countdown's first-poll race even tighter. The portal task is created
with `xTaskCreate` (no core pin) and the HTTP server is single-accept,
single-handle. Analysis incoming separately — likely candidates are
core-affinity contention with the MQTT broker task and head-of-line
blocking from one slow request. See follow-up notes in chat.
