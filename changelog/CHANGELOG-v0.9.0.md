# 0.9.0 — Berry scripting runtime

**Headline:** Embed Berry v1.1.0 as the broker's automation layer. Scripts
subscribe to MQTT topics, react to messages, make HTTP requests, and publish
back — all running on a dedicated FreeRTOS task without touching the broker.

---

## P0 — Correctness / regressions fixed

- **Timer Save button sent empty POST body.** The main timer form had no
  `id` attribute; the Save button's `form='timer-save'` reference found no
  target and submitted an empty wrapper form. Unchecking `arm` or `repeat`
  had no visible effect. Fixed by adding `id='timer-save'` to the form.

- **Berry `dispatch_topic` stack corruption.** `be_getindex()` pushes the
  result without popping the key. The original loop called `be_pop(1)` after
  each `be_getindex`, leaving the integer key on the stack — causing the
  next container lookup to index into an integer instead of the pair list.
  Callbacks never fired. Fixed with `be_remove(vm, -2)` after every
  `be_getindex`. (`berry_runtime.c`, `berry_mod_mqtt.c`)

- **Edit button opened wrong slot** (`btoggle` used inline style check).
  `element.style.display === ''` is empty for CSS-class-hidden elements.
  Fixed to use `getComputedStyle().display`.

- **`/berry` page had no Main Menu button.** Added consistent with every
  other portal page.

---

## P1 — New features

### Berry VM (Phases 1–4)

- **`components/berry/`** — vendored Berry v1.1.0 as an IDF component.

- **`mqtt` native module** (P3):
  - `mqtt.subscribe(filter, fn)` — register a callback for matching topics
  - `mqtt.unsubscribe(filter)` — remove matching subscriptions
  - `mqtt.publish(topic, payload [, qos=0 [, retain=false]])` — routes
    through `broker_publish_local()`, same queue used by timers
  - `mqtt_broker.c` calls `berry_publish_topic_event()` after every fanout

- **`http` native module** (P4):
  - `http.get(url [, timeout_ms=5000])` → `[status, body]`
  - `http.post(url, body [, content_type [, timeout_ms]])` → `[status, body]`
  - Returns a Berry list: `r[0]` = HTTP status code, `r[1]` = body string
  - Error case: `r[0] == -1`, `r[1]` = error description
  - Response body capped at 4 KB. **Two supported formats: JSON
    (`json.load(r[1])`) or plain text (`r[1]` as-is).** No binary, no XML.
  - Synchronous on `berry_task`; broker never stalls.

### Multi-slot script manager

- **4 named script slots** (`berry_s0..s3_{nm,sc,en}` in NVS `mqtt_cfg`).
  - Each slot: label (≤31 chars), script body (≤2000 bytes), enable toggle.
  - Enabled slots run sequentially in the shared VM at every boot/restart.
  - One-time migration: legacy `berry_en` + `berry_script` → slot 0 on first boot.

- **`/berry` portal page**:
  - Slot list with inline accordion editor (Edit / Cancel / Save)
  - Trash button (🗑) to clear a slot — disabled when slot is already empty
  - Inline Run-once REPL (result injected in-page, textarea preserved)
  - Auto-refreshing log pane (2 s poll)

### Portal UX

- **Main menu grouped** into labelled sections:
  `NETWORK` · `BROKER` · `SYSTEM` (includes Restart)

---

## P2 — Polish

- **`examples/berry/`** — two copy-paste-ready example scripts with README:
  - `tasmota_power_state.be` — subscribe to MQTT POWER topic, query on boot
  - `tasmota_http_get.be` — HTTP GET + `json.load()` parsing, live tested

---

## Out of scope / backlog

Lifecycle hooks (P5), `watchdog` + `kv` + `log()` (P6), budget/heap cap (P7),
WebSocket live-stream on `/ws/berry` (P8). See `plan-berry-scripting.md §8`.

---

## Flash impact

| Baseline | Size | OTA slot free |
|----------|------|---------------|
| 0.8.3 | `0x120520` (1.13 MB) | 72% |
| 0.9.0 | `0x13c4f0` (1.24 MB) | 69% |
| Delta | **+116 KB** | — |

Dominated by the Berry VM (~80 KB). Pre-justified in `plan-berry-scripting.md §8`.

---

## Verification trail

1. `idf.py build` — clean, only pre-existing `-Woverride-init` noise.
2. `make ota` → device rebooted cleanly on 0.9.0, `firmware.version = "0.9.0"`.
3. Berry slot 1 "tasmota power": MQTT subscribe + query → `Tasmota power state: ON/OFF` ✓
4. `http.get('http://192.168.22.238/cm?cmnd=Power')` → `[200, '{"POWER":"OFF"}']` ✓
5. `http.post('http://192.168.22.238/cm?cmnd=Power', '')` → `[200, '{"POWER":"OFF"}']` ✓
6. Bad host → `[-1, 'connect error: ESP_ERR_HTTP_CONNECT']` ✓
7. Timer Save: unchecked arm + repeat → saved correctly (Playwright) ✓
8. `/berry` Edit button opens slot editor; Cancel closes; trash clears (Playwright) ✓
9. Main menu groups: NETWORK / BROKER / SYSTEM visible (Playwright) ✓
