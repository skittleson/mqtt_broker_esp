# Plan: MQTT Topic Tester in Web UI

**Target device:** http://192.168.22.100/ (live, has connected MQTT clients)
**Deployment:** OTA upload only — no serial flash, no reset.
**Risk profile:** HIGH. Every change ships through the same broker that real clients depend on.

---

## 0. Guiding constraints (read before every step)

1. **The broker is a single-threaded `select()` loop** (`broker_task`, `mqtt_broker.c:740`, pinned to core 1). No internal mutexes exist today because all client state is touched only by that one task. **Any cross-task call into the broker must add explicit synchronization or use a thread-safe queue — do NOT just call broker functions from the portal task.**
2. **The portal is a hand-rolled raw-socket HTTP server** (`portal_http_task`, `portal.c:1732`). It is single-accept, blocking. There is **no `esp_http_server`**, so **WebSocket framing (RFC 6455) must be implemented by hand**: SHA-1 + Base64 handshake, 2/8/14-byte frame headers, mask XOR, ping/pong, close. No library shortcuts.
3. **OTA is the only way to ship.** Every build must (a) boot, (b) keep the existing portal reachable, (c) keep port 1883 broker accepting clients, so a follow-up OTA is always possible. If a build bricks the portal, recovery requires physical access.
4. **Live clients are connected right now.** Do not change wire format, retained-store layout, MQTT parsing, client slot count, or the broker's select timeout. The tester is **additive only**.
5. **Memory is tight.** ESP32-S3 with PSRAM but broker uses it; budget the tester at ≤ ~8 KB SRAM steady state (ring buffer + 1 WS client struct). Hard cap WS clients (start with **2**).
6. **No frameworks.** UI is vanilla HTML/JS, inline in `portal.c` like the rest of the pages.

---

## 1. Pre-flight (do not skip)

- [ ] **1.1** Confirm current firmware version on `192.168.22.100` (read `/` or `version.h` route) and record it here: `__________`.
- [ ] **1.2** Pull a fresh OTA backup: `curl http://192.168.22.100/ota-download -o backup-$(date +%F).bin` if a download route exists; otherwise note current `version.h` hash and keep the built `.bin` from the last good build (`build/mqtt_esp32.bin`) archived as `rollback.bin`.
- [ ] **1.3** Verify rollback procedure works on a **bench device first** (separate ESP32-S3, not the live one). Flash `rollback.bin` via OTA and confirm it boots.
- [ ] **1.4** List currently connected MQTT clients (via existing portal stats page if present, or `mosquitto_sub -h 192.168.22.100 -t '$SYS/#'` if `$SYS` topics are exposed). Record count: `__________`.
- [ ] **1.5** Snapshot retained-message count and total bytes (portal stats page). Record: `__________`.
- [ ] **1.6** Confirm a bench ESP32-S3 is available and reachable for **every** intermediate OTA. **Do not OTA the live device until step 8.**

---

## 2. Design lock-in (already decided — do not re-litigate mid-build)

| Area                  | Decision                                                                          |
| --------------------- | --------------------------------------------------------------------------------- |
| Transport             | WebSocket on portal port 80, path `/ws`                                           |
| Tester client model   | Virtual internal client inside `mqtt_broker` (does not consume a TCP client slot) |
| Publish payload       | UTF-8 text only, retain checkbox, QoS forced to 0                                 |
| Subscribe history     | 50-message fixed ring buffer in broker, client-side filter in browser             |
| Concurrent WS clients | **2 max** (hard cap, reject 3rd with 503)                                         |
| Subscription scope    | Tester is always subscribed to `#` server-side; topic filtering is done in JS     |
| Auth                  | Reuses existing portal Basic auth on both `GET /tester` and `GET /ws`             |
| $SYS visibility       | Hidden by default in UI, toggle checkbox to show                                  |
| Rollout               | Single compile-time flag `BROKER_TESTER_ENABLED=1`. No runtime toggle.            |

---

## 3. Broker-side changes (`mqtt_broker.[ch]`) — **additive, behind a feature flag**

Add `#define BROKER_TESTER_ENABLED 1` in `mqtt_broker.h` so we can compile it out if anything goes sideways.

- [ ] **3.1** Add a **thread-safe SPSC ring buffer** for tester messages:
  - Struct: `{ char topic[64]; uint8_t payload[256]; uint16_t payload_len; uint8_t truncated; uint32_t seq; }`.
  - 50 slots = ~16 KB. If too large, drop to 32 slots or shorten payload to 192 B.
  - Producer: `broker_task` only.
  - Consumer: portal WS writer task(s).
  - Sync: `portMUX_TYPE` spinlock (FreeRTOS `taskENTER_CRITICAL`) OR a `StreamBuffer`. **Prefer `StreamBuffer`** — it's designed for this and is ISR/task safe. Frame each message with a length prefix.
- [ ] **3.2** Add a hook inside the existing PUBLISH fanout (search for where `retain_deliver` or per-subscriber send happens) so that **after** normal delivery to real subscribers, the broker also enqueues the message into the tester ring buffer. **Do NOT add this inside parse/validate — only after successful delivery, so a malformed packet path is untouched.**
- [ ] **3.3** Add `broker_tester_publish(const char *topic, const uint8_t *payload, size_t len, bool retain)`:
  - Called from the portal WS task.
  - Internally constructs an MQTT PUBLISH and routes it through the **same code path** that handles incoming PUBLISH from a real client, so retained-store + fanout semantics are identical.
  - Must acquire a broker-side lock or post to a queue consumed by `broker_task`. **Recommended:** post `{topic, payload, retain}` to a FreeRTOS queue that `broker_task` drains at the top of its select loop. This keeps all broker state mutation on the broker task. **No locks added to existing code paths.**
- [ ] **3.4** Add tester to broker stats output (count of msgs enqueued, count dropped due to full ring) for debugging without a debugger.
- [ ] **3.5** Unit-ish test on bench device only: connect `mosquitto_pub` and `mosquitto_sub` to the bench broker, fire 1000 msgs, confirm real subscribers still get every message and tester ring shows the last 50.

---

## 4. WebSocket implementation (`portal.c`) — the riskiest piece

- [ ] **4.1** Write `ws_handshake(int fd, const char *sec_websocket_key)` — RFC 6455 §4.2.2. SHA-1(`key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"`) → Base64 → `Sec-WebSocket-Accept`. ESP-IDF provides `mbedtls_sha1` and `mbedtls_base64_encode`. **Write unit-test vectors as comments next to the function** — the magic GUID and a known-good key/accept pair from the RFC.
- [ ] **4.2** Write `ws_recv_frame(int fd, uint8_t *opcode, uint8_t *buf, size_t buflen, size_t *out_len)`:
  - Handle 2-byte header, 7-bit / 16-bit / 64-bit payload length.
  - **Reject 64-bit lengths** (cap at 8 KB; close with 1009 "message too big").
  - Apply mask XOR (mask is required from client per spec).
  - Handle fragmentation: **reject fragmented frames** initially (close 1003). Browsers don't fragment small JSON.
  - Handle PING → reply PONG. Handle CLOSE → reply CLOSE and shut down.
- [ ] **4.3** Write `ws_send_frame(int fd, uint8_t opcode, const uint8_t *buf, size_t len)`:
  - Text frames only for messages, binary not needed.
  - Server-to-client frames are unmasked per spec.
- [ ] **4.4** In `portal_http_task`, detect `Upgrade: websocket` header on `GET /ws`. On match, perform handshake, then **spawn a dedicated task** `ws_client_task` (stack 4096, prio 4 — lower than broker). **Do not block the accept loop.** The accept loop must return to `accept()` immediately so other HTTP requests (including OTA!) keep working.
- [ ] **4.5** `ws_client_task`:
  - Reads from `StreamBuffer` (tester ring) and writes frames to socket.
  - Uses `select()` with short timeout on the socket to also read inbound publish requests.
  - JSON wire format (server → browser): `{"t":"<topic>","p":"<utf8 payload>","r":<retain>,"s":<seq>}`. Truncated payloads get `"trunc":true`.
  - JSON wire format (browser → server): `{"action":"publish","topic":"...","payload":"...","retain":false}`. **Validate strictly**: topic ≤ 64 B, payload ≤ 256 B, no NULs in topic, no wildcards in topic (per MQTT spec for PUBLISH).
  - On any parse error: send `{"error":"..."}` and continue. On socket error: close cleanly.
- [ ] **4.6** Hard limits enforced in `ws_client_task`:
  - Max 2 concurrent WS clients (global `atomic_int`). Reject 3rd with HTTP 503 **before** spawning task.
  - Max 20 publishes/sec per WS client (token bucket). Excess → `{"error":"rate_limit"}`.
  - Idle timeout 5 min → close 1000.
- [ ] **4.7** **Crash safety**: every malloc checked, every recv/send checked, task ends with `vTaskDelete(NULL)` after `close(fd)`. Never leak the WS client slot counter on early exit — use a single cleanup label.

---

## 5. UI page (`portal.c`)

- [ ] **5.1** Add nav link "MQTT Tester" alongside existing pages.
- [ ] **5.2** New route `GET /tester` returns single HTML page. Inline CSS, inline JS. Sections:
  - **Publish form**: topic input, payload textarea, retain checkbox, "Publish" button.
  - **Subscribe pane**: topic filter input (client-side, supports `+` and `#` as substring match — call this out in helper text so users know it's not a real MQTT subscription), pause/resume button, clear button, scrolling list of messages with timestamp / topic / payload.
- [ ] **5.3** JS:
  - Open `new WebSocket("ws://" + location.host + "/ws")`.
  - Auto-reconnect with 2 s backoff capped at 30 s.
  - Keep last 200 messages in browser (independent of device ring).
  - Show connection state badge (green/red).
  - Escape all topic/payload text with `textContent`, never `innerHTML`. **XSS matters even on a LAN.**
- [ ] **5.4** Render payloads in a `<pre>` with `white-space: pre-wrap; word-break: break-all;` so long lines don't break layout.

---

## 6. Build & local verification (bench device)

- [ ] **6.1** `idf.py build` — clean build, no new warnings.
- [ ] **6.2** Check binary size growth: compare `build/mqtt_esp32.bin` before/after. If growth > 30 KB, investigate.
- [ ] **6.3** Flash bench device via serial (`idf.py -p /dev/ttyUSB0 flash monitor`).
- [ ] **6.4** Smoke test sequence on bench:
  1. Confirm portal loads, all existing pages render.
  2. Connect `mosquitto_sub -h <bench-ip> -t '#' -v` from desktop.
  3. Connect `mosquitto_pub -h <bench-ip> -t test -m hello`.
  4. Confirm desktop sub sees it.
  5. Open `/tester` in browser. Confirm WS connects (devtools network tab).
  6. Confirm `hello` appears (was retained? if so it replays from ring buffer).
  7. Publish from UI → confirm desktop `mosquitto_sub` receives it.
  8. Open 2 browser tabs → both work. Open 3rd → gets 503.
  9. Kill WS tab, reopen → reconnects cleanly. Slot counter returns to 0 (check broker stats).
  10. Hammer test: `for i in {1..500}; do mosquitto_pub -h <bench-ip> -t s/$i -m "msg $i"; done`. UI shows last 50, no crash, no leak (check free heap before/after via portal).
- [ ] **6.5** **OTA self-test on bench**: build a tiny no-op change (bump `version.h`), OTA-upload it to the bench device via `/ota-upload`. Confirm it reboots into the new build and the tester still works. This validates that the new firmware can OTA itself.
- [ ] **6.6** Leave bench device running for ≥ 2 hours with tester WS open and a `mosquitto_pub` loop. Check free heap doesn't trend down.

---

## 7. Pre-deployment review checklist

- [ ] **7.1** No code paths inside existing broker functions changed except the one fanout hook (step 3.2). Diff-review with care.
- [ ] **7.2** `BROKER_TESTER_ENABLED` can be `#define`'d to 0 and the build still produces a working broker with zero tester code linked in. Test this by building both ways.
- [ ] **7.3** All new public symbols are prefixed `broker_tester_` or `portal_ws_`.
- [ ] **7.4** No new `printf`/`ESP_LOGI` inside the broker hot path (fanout). Logging goes through `ESP_LOGD` so it's off at INFO level.
- [ ] **7.5** Stack sizes: WS task = 4096, verified with `uxTaskGetStackHighWaterMark` during smoke test.
- [ ] **7.6** Confirm `/ota-upload` still works **after** opening a WS connection (some hand-rolled HTTP servers break when concurrent connections show up — test it).
- [ ] **7.7** Archive the exact `.bin` that's going to ship as `releases/tester-v<X>.bin`. Tag git.

---

## 8. Live deployment to 192.168.22.100

- [ ] **8.1** Announce maintenance window to anyone using the broker. Even though tester is additive, OTA reboots the device → all MQTT clients reconnect.
- [ ] **8.2** Verify `rollback.bin` is on the laptop, on disk, with known-good SHA.
- [ ] **8.3** Open two terminals: one running `mosquitto_sub -h 192.168.22.100 -t '#' -v` (will disconnect on reboot — that's the canary for "broker came back"), one ready to OTA the rollback.
- [ ] **8.4** Navigate to `http://192.168.22.100/ota-upload`, upload new `.bin`.
- [ ] **8.5** Wait for reboot. Within 30 s the `mosquitto_sub` should reconnect and start receiving again. **If it does not reconnect within 90 s, the device is wedged → physical recovery needed; OTA cannot help.**
- [ ] **8.6** Hit `http://192.168.22.100/` — confirm version string bumped.
- [ ] **8.7** Hit `http://192.168.22.100/tester` — confirm WS connects, see live traffic from real clients flowing through the subscribe pane.
- [ ] **8.8** Publish a benign test message from the tester (`tester/hello`). Confirm `mosquitto_sub` from step 8.3 sees it.
- [ ] **8.9** Watch for **10 minutes**: free heap stable, no client disconnect storms, retained count unchanged unless intentionally published.

---

## 9. Rollback procedure (rehearse before step 8)

If at any point after 8.4 the device misbehaves:

1. Open `http://192.168.22.100/ota-upload`.
2. Upload `rollback.bin`.
3. If portal is unreachable: there is no remote recovery — you need physical USB access. **This is the worst case the entire plan is designed to avoid.**

If portal is reachable but tester is broken and broker is fine:

- Leave it. File a bug. Tester is non-critical.

If broker is broken (clients can't connect):

- Immediate rollback per above.

---

## 10. Post-deploy

- [ ] **10.1** Update `README.md` with a "MQTT Tester" section.
- [ ] **10.2** Document the WS JSON protocol in `docs/`.
- [ ] **10.3** File follow-ups: binary payload support, real server-side topic filter, multi-message publish, JSON pretty-print.

---

## Open questions to resolve before coding starts

1. Is there already an authentication layer on the portal? If anyone on the LAN can hit `/tester`, anyone can publish anything. **Confirm this is acceptable** or add a basic-auth gate reusing whatever the existing OTA upload uses (if any).
2. Does the broker currently expose `$SYS/#` topics? If yes, the tester will show them by default and may flood the UI — add a default client-side filter `!$SYS/`.
3. Confirm exact ESP-IDF version and that `mbedtls_sha1`/`mbedtls_base64_encode` are in the default sdkconfig (they are in v5.5 by default, but worth a `grep CONFIG_MBEDTLS sdkconfig`).
