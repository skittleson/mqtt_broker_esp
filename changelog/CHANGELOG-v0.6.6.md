# mqtt_broker v0.6.6 — Portal latency: A + E + F

Built: May 13 2026 · SHA-256:
`d15ba7fc35f07c86a737534a704a2cf4d9e1046dafc03257222ba76c5f75f47f`

Closes the user's request from chat: pin the portal to a core, raise the
listen backlog, add a per-request access log. Implements options A, E,
and F from `docs/portal-latency-analysis.md`.

## What changed

### A — Core pinning

```c
- xTaskCreate(portal_http_task, "portal_http", PORTAL_TASK_STACK, NULL, 5, NULL);
- xTaskCreate(portal_dns_task,  "portal_dns",  PORTAL_TASK_STACK, NULL, 5, NULL);
+ xTaskCreatePinnedToCore(portal_http_task, "portal_http",
+                         PORTAL_TASK_STACK, NULL, 5, NULL, 0);
+ xTaskCreatePinnedToCore(portal_dns_task,  "portal_dns",
+                         PORTAL_TASK_STACK, NULL, 5, NULL, 0);
```

Per-WS tasks (`portal_ws.c`) get the same treatment. The MQTT broker
stays pinned to CPU 1. Result: the portal no longer round-robins with
the broker on equal-priority 10 ms tick slices.

### E — listen() backlog

```c
- listen(s_http_fd, 4);
+ listen(s_http_fd, 8);
```

Matches what `mqtt_broker.c` already uses. Stops dropping the 5th/6th
parallel SYN from modern browsers.

### F — Per-request access log

```c
static void handle_http_client(int client_fd) {
    int64_t req_start_us = esp_timer_get_time();
    const char *req_method = "?", *req_path = "?";
    /* ... handle ... */
    int64_t elapsed_ms = (esp_timer_get_time() - req_start_us) / 1000;
    if (elapsed_ms >= 25) {
        ESP_LOGW(TAG, "http  %s %s  done  %lldms  (slow)",
                 req_method, req_path, elapsed_ms);
    } else {
        ESP_LOGD(TAG, "http  %s %s  done  %lldms",
                 req_method, req_path, elapsed_ms);
    }
}
```

Logging policy:

- **Fast (<25 ms)**: `ESP_LOGD` — compiled out at the default verbosity.
  Pushing every fast line to the console *would itself add ~5–10 ms* to
  the request and ruin the very metric we're trying to measure.
- **Slow (≥25 ms)**: `ESP_LOGW`. Stays visible in serial output even
  with default log level. Threshold matches the fast-path baseline on
  0.6.5 with no log line in the way.
- **401**: `ESP_LOGW`. Helps users debug auth-related issues without
  enabling debug verbosity.

Also added matching warnings for `ENOMEM` (`malloc` failure on the recv
buffer) and `recv_fail` (slow/aborted client closing before headers
arrive) so those paths get accounted for too.

## Three build-system gotchas hit (worth recording)

1. **`build/mqtt_broker.bin` was stale after the first `idf.py build`.**
   IDF's CMake didn't pick up `portal.c`/`portal_ws.c` edits I'd made
   moments before the build started; the same 0.6.4 binary got
   "OTA-flashed" as 0.6.5 and the device kept reporting 0.6.4 after
   reboot. Fixed by manually removing the stale `.obj` files and
   rebuilding. Worth adding a `CMAKE_CONFIGURE_DEPENDS` on `portal.c`
   the same way version.h is handled if this happens again.

2. **`esp_timer.h` not found.** `esp_timer_get_time()` requires
   `#include "esp_timer.h"` AND `esp_timer` in `main/CMakeLists.txt`'s
   `REQUIRES` list. Easy missed step; the IDF v5 component model is
   strict about explicit declarations.

3. **OTA "success" but no version flip.** Symptom of (1): the OTA upload
   succeeded byte-for-byte, but the bytes were identical to the running
   image, so the device booted back to "the same version." Caught by
   `strings build/mqtt_broker.bin | grep '0\.6\.'` showing the wrong
   number after CMake claimed `PROJECT_VER=0.6.5`.

## End-to-end measurement

Setup: 6 Tasmota clients connected, normal telemetry traffic, broker
otherwise idle. `urllib.request.urlopen()` in a tight Python loop from
the developer machine on the same Ethernet LAN.

```
=== Baseline (0.6.4) ===
n=50/50  median=16.0ms  p95=129.3ms  max=136.0ms
histogram:  <25ms: 43   25-50: 1   50-100: 0   100-250: 6   >250: 0
                                              ^^^^^^^^^^^^
                                              12% in slow bucket

=== After A+E+F (0.6.6) ===  3 rounds * 30 requests
round 1  median=20.6ms  p95= 43.1ms  max= 47.6ms   slow (>=50ms): 0/30
round 2  median=19.5ms  p95= 64.1ms  max= 69.3ms   slow (>=50ms): 2/30
round 3  median=18.4ms  p95= 54.5ms  max= 65.1ms   slow (>=50ms): 1/30
aggregate: median ~19ms  p95 ~54ms  max ~70ms      slow: 3/90 (3%)
                                                   >100ms: 0/90 (gone)
```

Trade-off: median got slightly worse (+3 ms) because pinning to CPU 0
puts the portal on the same core as the WiFi driver and LwIP TCP/IP
task. Those run at much higher priority (18–23) so they preempt
cleanly, but the portal occasionally waits a tick for them. Cost is
real but small; the +3 ms median is barely noticeable in a UI that
polls every 1 s.

## Files changed

```
main/portal.c              +56  -6   (esp_timer include, access log,
                                       core pinning comments, backlog
                                       comment, 401 log)
main/portal_ws.c           +5   -2   (xTaskCreatePinnedToCore for WS)
main/version.h             +2   -2   (0.6.5 -> 0.6.6)
main/CMakeLists.txt        +1   -1   (REQUIRES += esp_timer)
docs/portal-latency-analysis.md  +56  -0  (After-the-fact section
                                            with the measured improvement)
README.md                  +18  -1   (What's new in 0.6.6 section,
                                       version table)
CHANGELOG-v0.6.6.md        new
releases/mqtt_broker-v0.6.6.bin  new
releases/mqtt_broker-v0.6.6.bin.sha256  new
```

Binary size: 1.13 MB → 1.13 MB (+0.3 KB net for the new log
infrastructure).

## What's next

The analysis doc still lists B (2-worker accept-and-handle pool), C
(`LWIP_TCPIP_CORE_LOCKING`), D (TCP/IP task stack 3072→4096), and G
(static page-body buffer). B is the highest-impact remaining change —
it's what would prevent the 20 s OTA upload from blocking every other
HTTP request during the flash window. C is the next-easiest win
(sdkconfig change, ~3–5 ms per request).
