# plan-berry-scripting.md — Embed Berry as the broker's rules engine

**Target release:** v0.9.0 (minor bump; additive NVS keys, OTA-clean from 0.8.x)
**Status:** planning — all major design decisions resolved via questionnaire,
ready for human review then phased implementation.

---

## 1. Goals

Embed the [Berry](https://github.com/berry-lang/berry) scripting language
as the broker's automation layer. Berry is a small (~40 KB code), Lua-like,
embedded-friendly VM originally designed for Tasmota. Reusing it lets the
broker stay a single C codebase while giving operators a real scripting
surface for the four target use cases:

1. **Topic → action triggers.** A topic fires a Berry callback that can
   publish other MQTT messages, hit HTTP endpoints, or update state.
2. **Counter → webhook.** Count matching topics, fire `http.post()` on the
   Nth event.
3. **One-shot init scripts.** Operator pastes Berry code into a portal
   "Run once" pane to walk every known client and send a config-publish
   on boot of a new deployment.
4. **Event hooks.** Named globals (`on_timer_fire`, `on_settings_change`,
   `on_client_connect`, `on_client_disconnect`, `on_boot`) the runtime
   calls when those events occur.
5. **Watchdog primitive.** `watchdog.expect(client, timeout_s, cb)` —
   if `client` is silent for `timeout_s`, call `cb()`.

### Non-goals (explicitly out of scope for v0.9.0)

- Multiple named script slots. v0.9.0 ships a single autoexec.be-style
  blob; slot management can come later without breaking storage.
- Filesystem partition for `.be` files. Stays in NVS for v0.9.0.
- Berry → flash/GPIO peripherals. This is an MQTT broker, not a Tasmota
  fork. No `gpio.*`, no `i2c.*`, no `light.*`. Keep the API surface
  scoped to MQTT + HTTP + broker lifecycle.
- Berry → TLS-cert-auth integration. Independent feature; punt to its
  own plan (`plan-tls-cert-auth.md`).

---

## 2. Resolved design decisions

| Axis              | Decision                                                                |
| ----------------- | ----------------------------------------------------------------------- |
| Scope             | Full Berry VM + all 4 use cases in one feature drop (v0.9.0).           |
| Script storage    | Single NVS blob `mqtt_cfg/berry_script`, edited in portal textarea.     |
| Threading         | Dedicated `berry_task` + FreeRTOS queue. Bug-isolated from broker_task. |
| API modules       | `mqtt`, `http`, lifecycle hooks, `watchdog` + `kv` + `log()`.           |
| Manual invocation | Portal /berry "Run once" button **and** `POST /api/berry/eval`.         |
| Safety            | Per-callback wall-clock budget (200 ms) + heap cap (32 KB).             |
| Observability     | WebSocket live-stream on `/berry` (reusing portal_ws infrastructure).   |
| Integration       | Vendored sources at `components/berry/`, pinned to upstream tag.        |
| Versioning        | Minor bump to **0.9.0**; additive NVS keys; no partition change.        |

---

## 3. Data model

### 3.1 NVS additions (namespace `mqtt_cfg`)

| Key               | Type | Default | Purpose                                                                   |
| ----------------- | ---- | ------- | ------------------------------------------------------------------------- |
| `berry_en`        | u8   | `0`     | Master enable. When 0, Berry task is not started.                         |
| `berry_script`    | str  | `""`    | The autoexec.be blob. ~12 KB hard cap to fit comfortably in NVS.          |
| `berry_heap_kb`   | u8   | `32`    | Berry VM heap cap. Tunable 16-64 KB.                                      |
| `berry_budget_ms` | u16  | `200`   | Per-callback wall-clock budget in milliseconds.                           |
| `berry_log_topic` | u8   | `0`     | If 1, also publish log lines to `$SYS/broker/berry/log` (off by default). |

Berry's own runtime `kv` store (use-case helper) lives in a **separate**
NVS namespace `berry_kv` so it can be wiped without touching broker config.

### 3.2 New NVS namespace `berry_kv`

Owned by `berry_runtime.c`. Free-form key/value store backing the Berry
`kv.set(key, value)` / `kv.get(key, default)` helpers.

- Keys: ASCII `[a-zA-Z0-9_]{1,15}` (NVS limit).
- Values: strings or 32-bit integers (two helper paths in NVS).
- Total budget: refuse `kv.set()` once partition free space < 4 KB.
- AGENTS.md §4 table will be updated in the implementing commit.

---

## 4. Code map (new files)

```
components/berry/                 Vendored Berry sources from upstream tag.
  CMakeLists.txt                  Thin IDF wrapper: COMPONENT_SRCS = all of src/*.c
                                  excluding default/, plus our overrides.
  src/                            Pristine upstream (do not modify in-tree).
  port/                           Local overrides — be_port.c, be_sys.c stubs.
  README.md                       Upstream tag/commit + local patch log.

main/
  berry_runtime.c/h               Owns the Berry VM, berry_task, queue, ring
                                  buffer, observe-hook budget enforcement.
                                  Exports berry_init(), berry_eval(),
                                  berry_publish_event(evt, payload).
  berry_mod_mqtt.c                Native module `mqtt`: subscribe(topic, fn),
                                  publish(topic, payload, qos, retain).
                                  Reuses tester publish queue (see §6.2).
  berry_mod_http.c                Native module `http`: get/post async.
                                  Uses esp_http_client on berry_task.
  berry_mod_watchdog.c            watchdog.expect(client, timeout, cb).
                                  Hooks broker_task's per-client last_seen.
  berry_mod_kv.c                  kv.get/set/delete backed by `berry_kv` NVS.
  berry_mod_log.c                 log(level, msg) → ring buffer + WS + optional $SYS.

  portal_berry.c                  /berry route handlers (page render, save,
                                  run-once, eval API, WS endpoint). Splits
                                  portal.c so it doesn't grow past ~4000 lines.
  portal_ws.c                     Existing; gains a second WS endpoint
                                  /ws/berry alongside /ws/tester. Refactor
                                  the path dispatch in portal_ws_handshake.

  main.c                          Add berry_init() after broker_start(),
                                  before timers_start().
```

`components/berry/` lives outside `main/` per IDF convention so the
broker's own warnings/CFLAGS don't apply to upstream code. Berry compiles
clean with `-Wno-old-style-declaration -Wno-unused-parameter` overrides
in its CMakeLists.

---

## 5. HTTP / API surface

All routes require Basic Auth (existing) + CSRF for state-changers.

| Method | Path                 | Body / Query                  | Purpose                                                      |
| ------ | -------------------- | ----------------------------- | ------------------------------------------------------------ |
| GET    | `/berry`             | —                             | HTML editor + log pane + Run-once textarea.                  |
| POST   | `/berry/save`        | form: `script`, `en`, `csrf`  | Persist autoexec blob + enable flag. Restarts Berry task.    |
| POST   | `/berry/eval`        | form: `script`, `csrf`        | Run a one-shot snippet; HTML response with stdout + result.  |
| GET    | `/api/berry/status`  | —                             | JSON: `{enabled, heap_kb, heap_used, last_error, uptime_s}`. |
| POST   | `/api/berry/eval`    | text/plain: script (CSRF hdr) | Run snippet; JSON `{ok, result, log:[...], error?}`.         |
| POST   | `/api/berry/restart` | empty + CSRF                  | Tear down + re-init the VM (re-runs autoexec).               |
| GET    | `/ws/berry`          | WS upgrade                    | Live-tail log + eval REPL frames (see §5.1).                 |

### 5.1 WebSocket protocol on `/ws/berry`

Client → server: JSON `{op:"eval", id:N, src:"..."}`
Server → client (text frames):

- `{type:"log", ts, level, msg}` — every log() / runtime error.
- `{type:"result", id:N, ok:true|false, value:"...", error?:"..."}`
- `{type:"event", evt:"on_topic"|..., topic?, client?, ms}` — optional firehose for debugging.

Reuses the existing portal_ws frame helpers. Path dispatch in
`portal_ws_handshake` switches between `/ws/tester` and `/ws/berry`.

---

## 6. Threading & event flow

### 6.1 Tasks

```
broker_task (existing, CPU 0)
  └─ on relevant event, calls berry_publish_event(evt_kind, payload)
     which is just xQueueSend to s_berry_queue.

berry_task (new, CPU 1, stack 8 KB)
  └─ for(;;) xQueueReceive(s_berry_queue, &evt, portMAX_DELAY)
     └─ dispatch:
        evt.kind == EVT_TOPIC        → call all subscribers matching evt.topic
        evt.kind == EVT_TIMER        → call on_timer_fire(slot, label)
        evt.kind == EVT_CLIENT_UP    → call on_client_connect(client_id)
        evt.kind == EVT_CLIENT_DOWN  → call on_client_disconnect(client_id, reason)
        evt.kind == EVT_SETTINGS     → call on_settings_change(section)
        evt.kind == EVT_EVAL         → run snippet, post result back to WS
        evt.kind == EVT_TICK_1HZ     → watchdog scan + on_tick (optional)
     └─ each dispatch runs under be_observe_hook with deadline = now + budget_ms
     └─ uncaught error → log to ring buffer + WS; never crash the task.

portal_http_task (existing, CPU 0)
  └─ /berry POST → posts EVT_EVAL onto s_berry_queue, waits with timeout on
                   a per-eval response semaphore.
```

### 6.2 Publish path — reusing existing infrastructure

Berry's `mqtt.publish()` does **not** open a TCP loopback to itself.
It posts to the existing **tester publish queue** in `mqtt_broker.c`
(the same one `portal_ws` and `timers.c` use). This is the discipline
called out in `AGENTS.md §5.2` — audit first, reuse second.

Effects:

- Berry → broker fanout reuses the proven retained-store, QoS, and
  $SYS path with zero new state.
- Berry's own subscriptions are an **in-process callback table**, not a
  TCP client. `mqtt.subscribe(topic, fn)` adds an entry to a Berry-side
  topic → fn list; broker_task's fanout loop checks this table after
  the existing subs[] walk and posts an EVT_TOPIC into s_berry_queue.

### 6.3 Watchdog

`watchdog.expect(client_id, timeout_s, cb)` records a tuple in a
Berry-task-owned table. Every 1 Hz EVT_TICK, the task checks each
entry against the broker's `clients[i].last_activity_ms` (read via
a thread-safe `broker_get_client_last_activity()` accessor — new,
small, returns a copy under the broker mutex). If overdue, fire `cb`
and remove the entry (re-arm requires calling `expect()` again).

---

## 7. Safety / sandbox

- **Wall-clock budget.** Berry's `be_observe_hook` fires every N
  bytecode instructions; we check `xTaskGetTickCount()` against the
  deadline set when the callback was entered. If exceeded, raise a
  Berry exception, which propagates up to the dispatch loop and gets
  logged as `"callback exceeded 200 ms budget"`.
- **Heap cap.** Berry's allocator goes through `be_realloc()` — we wrap
  it with `berry_alloc()` that tracks total bytes in use and refuses
  allocations past `berry_heap_kb * 1024`. Allocation failure raises
  a Berry exception; the offending callback dies, runtime continues.
- **No globals leak across `eval()` calls in REPL mode** — REPL evals
  run in a child scope and the result is discarded except for the
  display value. The persistent autoexec script does get a long-lived
  global scope.
- **HTTP client time-out.** `http.get/post` accepts an optional
  `timeout_ms` (default 5000). esp_http_client is given that hard cap.
- **Recursion guard.** Berry already has its own call-stack limit;
  default ~256 frames is plenty and protects against runaway recursion.

---

## 8. Phasing (effort + flash budget per phase)

Implementation order is designed so each phase is independently shippable
and OTA-verifiable, even though all four use cases land in 0.9.0.

| Phase | Scope                                             | Effort | Flash Δ est. | Risk |
| ----- | ------------------------------------------------- | ------ | ------------ | ---- |
| P1    | Vendor `components/berry/`, link clean, REPL only | 1d     | +50-70 KB    | M    |
| P2    | NVS persistence + autoexec + `/berry` page + WS   | 1d     | +6-10 KB     | M    |
| P3    | `mqtt` module + topic-subscribe callback table    | 1d     | +4-6 KB      | M    |
| P4    | `http` module (async esp_http_client)             | 0.5d   | +3-5 KB      | L    |
| P5    | Lifecycle hooks (timer/settings/client/boot)      | 0.5d   | +2-3 KB      | L    |
| P6    | `watchdog` + `kv` + `log` helpers                 | 0.5d   | +3-5 KB      | L    |
| P7    | Budget + heap-cap enforcement, error UX polish    | 0.5d   | +1-2 KB      | L    |
| P8    | Docs, screenshots, changelog, version bump        | 0.5d   | ~0           | L    |

**Total flash delta budget: ~70-100 KB.** That's well above the 20 KB
"justify it in the commit message" threshold in AGENTS.md §7.1 — this
plan document is that justification, and the per-phase commits will
each cite it.

Current OTA slot is 4 MB with 72% free at 0.8.2, so absorbing ~100 KB
brings it to roughly 69% free. Still ample.

---

## 9. Test strategy

### 9.1 Unit / on-device smoke

- `idf.py build` clean with no new warning classes beyond the existing
  `-Woverride-init` noise. Berry-internal warnings suppressed via
  CFLAGS in `components/berry/CMakeLists.txt`.
- After each OTA: `curl /api/berry/status` returns 200, `enabled:0`.
- Set `berry_en=1`, save a one-line `log("hello")` autoexec, restart
  Berry, confirm the ring buffer / WS shows it.

### 9.2 pytest additions (`test_berry.py`, new)

| Test                             | Asserts                                                   |
| -------------------------------- | --------------------------------------------------------- |
| `test_eval_basic`                | POST /api/berry/eval `1+1` → `{ok:true, value:"2"}`.      |
| `test_mqtt_publish_from_script`  | Eval `mqtt.publish("t/x","hi")`; mosquitto_sub sees it.   |
| `test_mqtt_subscribe_counter`    | Subscribe in script, mosquitto_pub 10×, verify HTTP fire. |
| `test_http_get_to_self`          | `http.get("http://127.0.0.1/api/ping", cb)`; cb runs.     |
| `test_on_client_connect_fires`   | mosquitto_sub connects; hook globals reflect the event.   |
| `test_watchdog_fires_on_silence` | Register watchdog, disconnect mosq, wait, see cb fired.   |
| `test_budget_exceeded_aborts_cb` | `while true: pass` callback gets aborted within budget.   |
| `test_heap_cap_raises`           | Allocate past cap → BerryError, runtime survives.         |
| `test_csrf_required_on_eval`     | POST without CSRF → 403.                                  |
| `test_eval_via_ws`               | WS frame `{op:"eval",src:"2+2"}` → `{type:"result",...}`. |

Target: 8-10 new assertions, keeping the existing ~129 baseline green.

### 9.3 Playwright UI capture

`tools/capture_berry.py` (new): renders `/berry` in three states
(empty, with script, post-error) into `docs/screenshots/berry/`.

### 9.4 Manual stress

Run an autoexec that does `mqtt.subscribe("#", fn)` with `fn` doing
`kv.set("count", kv.get("count",0)+1)`. Drive 1000 pubs/sec via
`stress_test.py`. Watch:

- `/api/clients` in-flight counter stays sane.
- Berry heap_used in `/api/berry/status` doesn't trend upward.
- No watchdog reset (CPU 1 idle stays > 10%).

---

## 10. Open questions / human review

1. **Upstream Berry tag to pin.** Latest release at time of writing is
   `v1.x.y` — pick a tag with the embedded port stable. To resolve in P1.
2. **`berry_kv` size limit.** Is 4 KB free-space floor right, or should
   we cap entry count instead? Defer; the floor is conservative.
3. **`on_settings_change(section)` granularity.** Should "settings"
   include NTP/wifi/timers, or just `mqtt_cfg`? Proposing: all three
   namespaces, with `section` = `"mqtt"|"ntp"|"wifi"|"timers"`.
4. **REPL history.** Persist last 20 eval snippets in `berry_kv` for
   the UI history dropdown? Cheap, but optional polish — defer to 0.9.1.
5. **`$SYS/broker/berry/state`.** Publish `{enabled, errors_24h, last_eval_ms}`
   periodically? Helpful for remote monitoring but adds a writer. Defer.
6. **OTA migration.** Should we pre-seed `berry_script` with a commented-
   out example script on first boot post-upgrade, or leave empty?
   Recommendation: empty + an `Examples` button on /berry that injects
   one of the four use-case templates into the editor.

---

## 11. Documentation deliverables

- `docs/berry.md` (new) — language quick-start, full module/function
  reference, all four use-case templates as copy-paste examples,
  troubleshooting section.
- `docs/api.md` — add the 6 new endpoints + WS endpoint.
- `docs/architecture.md` — task layout diagram updated to show
  berry_task and the queue.
- `AGENTS.md` — §3 code map, §4 NVS namespace table, §7.1 flash
  baseline row for v0.9.0.
- `changelog/CHANGELOG-v0.9.0.md` — full release notes.
- `CHANGELOG.md` — top-level paragraph.
- `main/version.h` — bump to 0.9.0 with a multi-line block comment
  describing Berry integration.

---

## 12. Verification trail (run before tagging v0.9.0)

1. `idf.py build` — clean, only pre-existing -Woverride-init warnings.
2. `make ota` against the live device at 192.168.22.100.
3. `curl -s -u support:dockyard http://192.168.22.100/api/status | jq .firmware` → `0.9.0`.
4. `BROKER_AUTH=support:dockyard make test` — full pytest suite, ≥137 assertions green (129 + ~8 new).
5. `tools/capture_berry.py` — new screenshots committed under `docs/screenshots/berry/`.
6. Manual: paste the "count to 10 → webhook" template into /berry,
   save+enable, drive 10 publishes via `/tester`, watch WS stream show
   the HTTP POST go out and receive a 200.
7. Manual: paste the "init all devices" template into Run-once, click
   Run, watch publishes appear in `/tester` topic list.
8. Manual: confirm a `while true: end` autoexec is killed by the
   budget enforcer within ~250 ms and the runtime logs the abort.
