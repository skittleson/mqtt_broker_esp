# Plan: Scheduled MQTT Publishes (Tasmota-Style Timers)

Status: Draft · Owner: TBD · Target: 0.6.0

A first-class **Timers** page on the portal that lets users schedule MQTT
publishes from the broker itself. Modeled directly on Tasmota's
`Configuration → Configure Timer` UI and JSON command surface, so anyone
coming from Tasmota recognizes the mental model instantly.

References:

- [Tasmota Timers docs](https://tasmota.github.io/docs/Timers/)
- [`xdrv_09_timers.ino`](https://github.com/arendst/Tasmota/blob/master/tasmota/xdrv_09_timers.ino)
- Portal UX baseline: `plan-mqtt-ux-v2.md`

---

## 1. Goals & non-goals

**Goals**

- 16 schedule slots, identical capability to Tasmota timers but with
  MQTT publish as the action (instead of `Output/Action` on a relay).
- Survive reboot (NVS-persisted, single JSON blob).
- Editable from the portal AND via MQTT command (`cmnd/<host>/Timer<n>`)
  for parity with Tasmota tooling.
- No JS required for the basic form (server-rendered) — Tasmota soul intact.
- Drift ≤ 2 s vs SNTP; firings are skipped (not stacked) if the broker was
  busy for > 60 s.

**Non-goals (this iteration)**

- Sunrise/Sunset modes (Phase 2 — requires lat/lon config).
- Sub-minute schedules (cron-style `*/10 * * * * *`). For "every 30 s"
  use a retained-publish loop in the client; this feature is **wall-clock**
  scheduling, not interval/heartbeat.
- Multiple actions per timer / rule engine. One timer = one publish.

---

## 2. Data model

Stored in NVS namespace `portal`, key `timers`, as a single JSON blob
(≤ 4 KB, fits comfortably in one NVS entry, atomic save). One entry per
slot, index 0..15.

```json
{
  "v": 1,
  "tz": "PST8PDT,M3.2.0,M11.1.0",
  "timers": [
    {
      "arm": 1,
      "mode": 0,
      "time": "07:00",
      "window": 0,
      "days": "-MTWTF-",
      "repeat": 1,
      "topic": "home/lights/cmd",
      "payload": "ON",
      "qos": 0,
      "retain": false,
      "label": "morning lights"
    },
    { "arm": 0 }
  ]
}
```

Per-slot fields (every field optional except as noted; defaults applied
server-side):

| Field     | Type   | Range / format                                                       | Tasmota equivalent | Notes                                                                                                     |
| --------- | ------ | -------------------------------------------------------------------- | ------------------ | --------------------------------------------------------------------------------------------------------- |
| `arm`     | int    | 0\|1                                                                 | `Arm`              | 0 = slot defined but inactive (kept for quick toggle).                                                    |
| `mode`    | int    | 0=Schedule, 1=Sunrise, 2=Sunset                                      | `Mode`             | Phase 2: sunrise/sunset. Phase 1 only accepts `0`.                                                        |
| `time`    | string | `HH:MM` 24h local                                                    | `Time`             | Local wall-clock per `tz_offset_min`. With mode 1/2, interpreted as offset (`+00:15`/`-01:00`) — Phase 2. |
| `window`  | int    | 0..15 (minutes)                                                      | `Window`           | Random jitter `±window` around `time`. Useful to de-thunder fleets.                                       |
| `days`    | string | 7 chars, Sun..Sat, e.g. `SMTWTFS`. `-` or `0` = off, any other = on. | `Days`             | Exactly Tasmota's encoding so `Timer1 {"Days":"--TW--S"}` round-trips.                                    |
| `repeat`  | int    | 0=once-then-disarm, 1=repeating                                      | `Repeat`           | Once-mode auto-clears `arm` after firing.                                                                 |
| `topic`   | string | ≤ 128 bytes, valid MQTT publish topic (no wildcards)                 | (new — Output)     | **Required when armed.**                                                                                  |
| `payload` | string | ≤ 256 bytes UTF-8                                                    | (new — Action)     | Empty payload allowed (valid MQTT). Hex escape via `\xNN` if needed.                                      |
| `qos`     | int    | 0 or 1                                                               | (new)              | QoS 2 not supported by broker; UI hides it.                                                               |
| `retain`  | bool   | true/false                                                           | (new)              | Maps to broker retained-message path.                                                                     |
| `label`   | string | ≤ 24 bytes UTF-8                                                     | (new)              | Free-text nickname, shown in the table. Pure cosmetic.                                                    |

Top-level fields:

- `v` — schema version (1). Bump on breaking change; loader migrates `v < 1` to defaults.
- `tz` — **POSIX TZ string** (see §2a). Replaces the previous fixed
  `tz_offset_min` setting, with a one-time migration on first boot of the
  new firmware (old offset → synthesized `UTC±HH:MM` string with no DST).

---

## 2a. Time zone & DST (the 10-year-lifetime question)

**Constraint:** the device must keep firing at the right local wall-clock
time for 10+ years. Governments change DST rules constantly (Brazil 2019,
Mexico 2022, EU pending). Hardcoding rules in firmware guarantees drift.

**Decision:** store a **POSIX TZ string** in NVS (`portal/tz`, default
`UTC0`), user-editable on `/settings`. Newlib + ESP-IDF resolve it natively:

```c
setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
tzset();
/* localtime(), mktime() now honor DST automatically. */
```

Why this works for 10-year lifetimes:

- ~50 bytes per zone; no IANA tzdata blob (which is ~2 MB) needed.
- When a country changes its rules, the **user** edits the string on the
  portal — no firmware reflash. The policy lives in config, not code.
- Already supported by `newlib` shipped with every ESP-IDF version since
  v4.0. Zero new code in the scheduler: it already does
  `localtime(&now)` to derive local minute/day, and that call honors DST.

**UI on `/settings` → Device tab:**

```
Time zone (POSIX TZ string):  [PST8PDT,M3.2.0,M11.1.0_________]
                              [ Pick from list ▼ ]   [?]
Current local time: 2026-05-18 14:32:08  (PDT, UTC-07:00)
Next DST transition: 2026-11-01 02:00 → 01:00 (PST)
```

- The `Pick from list` dropdown ships a **static** ~80-entry table (~6 KB
  in flash, `tz_presets.c`) covering all common zones. Each row writes a
  TZ string into the input; user can still edit it. Table is regenerated
  from current IANA tzdata at build time by `tools/gen_tz_presets.py` so
  every release ships current-as-of-build rules.
- `?` link to docs explaining the POSIX TZ format and pointing at
  <https://remotemonitoringsystems.ca/time-zone-abbreviations.php> for
  reference.
- **Next DST transition** is computed live: call `mktime` for Jan 1 +
  Dec 31, walk forward in 1-day steps comparing `tm_isdst` — cheap, runs
  once per page load.

**Transition correctness** (verified in tests, §9):

- **Spring forward** — a timer set for `02:30` on the skipped day:
  `mktime` for `02:30` of that day returns `-1` (no such local time).
  The scheduler detects this and **skips the slot for that day only**,
  logging `I (sched) slot %d skipped (DST spring forward gap)`.
  Matches Tasmota's behavior.
- **Fall back** — a timer at `01:30` on the duplicated day:
  `localtime()` returns each clock-minute exactly once when iterated 1 Hz
  on UTC, so the timer fires **once** at the first occurrence (the DST
  one). The existing "last fired minute (UTC)" dedup guard prevents a
  second fire when local time repeats. Documented.
- **`tz` change mid-flight** — on save, `tzset()` is called and the
  scheduler's "last fired UTC minute" cache is cleared for all slots
  (otherwise a backward shift could re-fire a slot in the new zone).

**Year 2038:** ESP-IDF newlib uses 64-bit `time_t` since v4.0 — confirmed
in `sdkconfig` (`CONFIG_NEWLIB_TIME_SYSCALL_USE_HRT` path). Not a concern.

**SNTP outage policy:** ESP32-S3 has no battery-backed RTC. If WiFi (or
Ethernet + upstream NTP) is down for the entire boot session, `time(NULL)`
stays at 0 and the scheduler simply doesn't fire — already covered by
the `< 1700000000` guard in §3. A red banner on `/timers` makes this
visible. **Documented limitation**, not a regression: matches Tasmota on
the same hardware class.

**Maintenance burden:** the `tz_presets.c` table is regenerated each
release from current IANA tzdata (one Python script, runs in CI). If a
device is never updated, presets get stale but the **user-typed** TZ
string still works — we never "resolve" a zone name to rules at runtime,
so there's nothing to go out of date inside the device.

**Capacity check:** worst case 16 slots × ~420 bytes ≈ 6.7 KB. Slightly over
the 4 KB NVS comfort zone. Mitigation: store as **compact JSON** (no whitespace,
abbreviated keys: `a, m, t, w, d, r, tp, pl, q, rt, l`). Worst case drops to
~3.6 KB. Internal struct uses full names; serializer expands/contracts.

---

## 3. Scheduler runtime

New module `main/timers.c` / `main/timers.h`.

```c
typedef struct {
    uint8_t  arm     : 1;
    uint8_t  mode    : 2;   // 0 schedule, 1 sunrise, 2 sunset
    uint8_t  repeat  : 1;
    uint8_t  days;          // bit0 Sun .. bit6 Sat
    int16_t  minute_of_day; // 0..1439, or signed offset for sunrise/sunset
    uint8_t  window;        // 0..15
    uint8_t  qos      : 1;
    uint8_t  retain   : 1;
    char     topic[129];
    char     payload[257];
    uint16_t payload_len;
    char     label[25];
} timer_slot_t;

esp_err_t timers_load(void);                       // NVS → RAM at boot
esp_err_t timers_save(void);                       // RAM → NVS, debounced 1 s
const timer_slot_t *timers_get(int slot);          // 0..15
esp_err_t timers_set(int slot, const timer_slot_t *t); // validates + saves
void      timers_tick(time_t now_utc);             // called once/sec by task
uint32_t  timers_next_fire_unix(int slot);         // for UI "next: in 3h 12m"
```

**Task** `timers_task` (priority 5, 3 KB stack, pinned to PRO_CPU):

- Wakes every 1000 ms via `vTaskDelayUntil`.
- Reads `time(NULL)`. If `< 1700000000` (pre-2023, SNTP not synced yet),
  skips evaluation entirely — never fires until clock is real.
- Converts UTC → local using `tz_offset_min`.
- For each armed slot: derive today's `target_min = minute_of_day +
rand_jitter(window)`. Fire iff:
  - current local minute == target_min, AND
  - we have not already fired this slot this minute, AND
  - current local day-of-week bit is set in `days`.
- Window jitter computed once per day per slot (seeded from
  `slot_idx ^ yday`) so it's stable within a day but varies day-to-day.
- After fire: call `mqtt_broker_publish_local(topic, payload, payload_len,
qos, retain)` — a **new public API on the broker** that injects a publish
  as if a client did it, going through retained-message + fanout paths.
  - Returns immediately if publish queue full; logs `W (sched) slot %d
dropped, queue full`. Never blocks the tick.
- If `repeat == 0`: clear `arm`, schedule a `timers_save()`.

**Catch-up policy:** if `now - last_tick > 60 s` (we were busy / just
booted / NTP just jumped), do **not** retro-fire missed timers. Log a single
`W (sched) clock jumped 67s, suppressing catch-up`. Tasmota does the same.

**Concurrency:** broker publish API needs to be safe to call from a
non-broker task. Audit `mqtt_broker_publish_local` for mutex coverage on
client list / retained map / subscription tree. If not already safe, add
a recursive mutex around the fanout — likely needed anyway for the
existing `$SYS/` publisher.

---

## 4. Broker integration

New public function in `main/mqtt_broker.h`:

```c
/* Inject a publish from a non-client source (scheduler, $SYS, future rules).
 * Goes through normal retained-storage + fanout. Thread-safe.
 * Returns ESP_OK on enqueue, ESP_ERR_NO_MEM if subscriber queue full. */
esp_err_t mqtt_broker_publish_local(
    const char *topic, const void *payload, size_t len,
    uint8_t qos, bool retain);
```

If `$SYS` publishes already use such a path (`mqtt_broker.c` likely has
something like `broker_publish_internal`), this just exposes it. Audit first.

**Topic safety:** publish topics with leading `$` are rejected (per MQTT
3.1.1 §4.7.2). The validator runs both at API-set time and at fire time
(defensive in case NVS was hand-edited).

---

## 5. HTTP / API surface

All new endpoints behind the existing portal-auth + CSRF middleware (see
`plan-mqtt-ux-v2.md` Phase 4).

### Pages

- `GET /timers` — list view. Server-rendered table:

  ```
  +---------------------------------------------------------------+
  | Timers                                              [+ Edit] |
  +---------------------------------------------------------------+
  | # | On | Label         | Time   | Days    | Topic       | Next |
  +---------------------------------------------------------------+
  | 1 | ●  | morning light | 07:00  | -MTWTF- | home/lt/cmd | 14h  |
  | 2 | ●  | lights off    | 22:00  | SMTWTFS | home/lt/cmd | 9h   |
  | 3 | ○  | (empty)       |   —    |    —    |     —       |  —   |
  | ...                                                            |
  +---------------------------------------------------------------+
  | Time source: SNTP synced · Local 14:32  (UTC-08:00)            |
  +---------------------------------------------------------------+
  ```

  Each row is a link to `/timers/<n>`.

- `GET /timers/<n>` — edit form for slot `n` (1..16, 1-based to match
  Tasmota). Form layout mirrors Tasmota exactly:

  ```
  +---------------------------------------------------------------+
  | Timer 1                                                       |
  | Label:    [morning lights______]                              |
  | [✓] Arm    [ ] Repeat                                         |
  | Mode:  (•) Schedule  ( ) Sunrise [disabled]  ( ) Sunset [..]  |
  | Time:  [07] : [00]    Window: [0  ]  min   (±0..15)           |
  | Days:  [✓]Sun [✓]Mon [✓]Tue [✓]Wed [✓]Thu [✓]Fri [✓]Sat       |
  | --- Publish action -----------------------------------------  |
  | Topic:    [home/lights/cmd_____________________]              |
  | Payload:  [ON__________________________________]  17/256 B    |
  | QoS:      ( ) 0  (•) 1                                        |
  | [ ] Retain                                                    |
  +---------------------------------------------------------------+
  |               [Save]   [Save & test fire now]   [Cancel]      |
  +---------------------------------------------------------------+
  ```

  - "Save & test fire now" POSTs `Save` then queues a one-off fire 2 s later
    (so the redirect can complete first). Banner on redirect: `Fired Timer 1
at 14:32:08 → home/lights/cmd`.
  - `Cancel` does a GET back to `/timers`.

- `POST /timers/save` — form action. Validates, calls `timers_set`,
  redirects to `/timers?saved=<n>`. Toast on redirect (server-rendered,
  consistent with Phase 2.6 of the v2 plan).

- `POST /timers/clear?n=<n>` — wipes a slot (`arm=0`, all fields default).
  Confirm via JS `confirm()`; non-JS fallback is a tiny `<form>` per row.

### JSON API

- `GET /api/timers` →

  ```json
  {
    "schema": 1,
    "tz_offset_min": -480,
    "now_unix": 1731940320,
    "timers": [
      {
        "n": 1,
        "arm": 1,
        "mode": 0,
        "time": "07:00",
        "window": 0,
        "days": "-MTWTF-",
        "repeat": 1,
        "topic": "home/lights/cmd",
        "payload": "ON",
        "qos": 0,
        "retain": false,
        "label": "morning lights",
        "next_fire_unix": 1731960000
      }
    ]
  }
  ```

- `PUT /api/timers/<n>` — JSON body identical to Tasmota's `Timer<n>` payload
  plus `topic/payload/qos/retain/label`. Validates and persists. Returns
  the canonicalized record. CSRF required (`X-CSRF` header).

- `DELETE /api/timers/<n>` — clear slot.

- `POST /api/timers/<n>/fire` — test-fire now. Returns `{fired:true,
topic:"...", at_unix: ...}`. Rate-limited 1/sec/slot (anti-rule-loop).

### MQTT command surface (Tasmota parity, optional Phase 1.5)

If the broker exposes a `cmnd/<hostname>/` topic (it doesn't today —
this is a new subscription owned by the scheduler module):

- `cmnd/<host>/Timer<n>` payload = JSON → same as `PUT /api/timers/<n>`.
- `cmnd/<host>/Timers` payload `0` disables ALL armed timers (master
  switch), `1` re-enables, empty payload returns JSON dump. Mirrors
  Tasmota's global-disable.
- Response published on `stat/<host>/RESULT` so existing tooling
  (Tasmoadmin etc.) can consume it.

Default `<host>` = `mqtt_broker` (the existing hostname setting). All
cmnd handling is **off by default** behind a Settings → MQTT → "Accept
Tasmota cmnd topics" checkbox so we don't surprise people.

---

## 6. UX details (matching the screenshots)

- **Nav button** added between `Tester` and the settings cog:

  ```
  | MQTT Broker  Home  Clients  Tester  Timers  ⚙ |
  ```

  Mobile: shrinks to `Timers` text, no icon, matches existing button width.

- **Status pill on `/timers`** when SNTP not synced:
  Red banner _"Clock not synced — timers are paused until SNTP succeeds.
  Last sync attempt: 14m ago."_ Links to `/time`.

- **Empty-slot rows** are rendered grey with `(empty)` label and an inline
  `[+ Configure]` button so the action is one click, not "click empty row →
  guess what to do".

- **Days picker**: 7 checkboxes, default all on. Quick links underneath:
  `Weekdays · Weekends · Every day · Clear` (pure `<a href>` with query
  params, no JS needed).

- **Window field**: number input, max=15, with helper text _"Random ±N min
  jitter; useful to stagger publishes across a fleet."_

- **Live "Next fire"** column: server-computed at page render. JS fetch
  every 30 s patches it in place (graceful degradation: without JS, you
  see the timestamp at the moment the page loaded).

- **Reveal payload**: if `payload` looks binary (contains bytes outside
  printable ASCII), the table shows `[hex] 7B 22 6F 6E ...`; the edit
  form keeps the raw bytes via a `Hex mode` toggle.

- **Toasts**: reuse the `?saved=<n>&at=<unix>` mechanism from the v2 plan,
  so a saved timer shows `Timer 3 saved · 14:32:08` for 3 s.

- **Accessibility**: every checkbox has `<label for>`, the days picker is
  a `<fieldset>` with `<legend>Days</legend>`. Hits Lighthouse a11y ≥ 95.

---

## 7. Validation rules

Server-side (the source of truth). Bad input → 400 with a JSON `{error:
"…"}` and a flash message on form POST.

- `time` matches `^([01]\d|2[0-3]):[0-5]\d$`.
- `days` is exactly 7 chars; each position is either `-`/`0` or a printable
  non-`-` non-`0` char (Tasmota's lax rule).
- `window` ∈ [0,15].
- `topic`: passes `mqtt_topic_validate_publish` (no `+`, no `#`, no leading
  `$`, no `\0`, ≤ 128 bytes, UTF-8).
- `payload` ≤ 256 bytes after decoding any `\xNN` escapes.
- `qos` ∈ {0,1}; `retain` ∈ {true,false}.
- `label` ≤ 24 bytes UTF-8 after trimming; control chars stripped.
- When `arm == 1`, `topic` must be non-empty. (Payload may be empty.)
- Slot index `n` ∈ [1,16].

Validators live in `timers.c` and are unit-testable on host.

---

## 8. Phasing

| Phase | Scope                                                                       | Effort |   Flash | Risk                                |
| ----: | --------------------------------------------------------------------------- | -----: | ------: | ----------------------------------- |
|   1.0 | Data model + `timers.c` + scheduler task + `mqtt_broker_publish_local`      |    2 d |   +3 KB | med (broker thread-safety audit)    |
|   1.1 | `/timers` list + `/timers/<n>` edit form + `/timers/save` + `/timers/clear` |    2 d |   +4 KB | low                                 |
|   1.2 | `/api/timers` JSON (GET/PUT/DELETE/fire) + tests                            |    1 d |   +1 KB | low                                 |
|   1.3 | Tasmota `cmnd/<host>/Timer<n>` MQTT command bridge                          |    1 d |   +1 KB | low (opt-in flag)                   |
|   1.4 | POSIX TZ string + `tz_presets.c` table + `/settings` Time-zone field        |    1 d |   +6 KB | low (newlib does the heavy lifting) |
|   2.0 | Sunrise/Sunset mode: lat/lon settings + solar calc (~300 LOC)               |    2 d |   +3 KB | low                                 |
|   2.1 | `?saved=` toast unification with v2 Phase 2.6                               |  0.5 d |      ~0 | low                                 |
|   2.2 | "Skipped fires" counter on `/timers` (debugging aid)                        |  0.5 d | +0.5 KB | low                                 |

Phase 1 total: **~7 dev-days, ~15 KB flash** (incl. TZ presets table).
Comfortably inside the OTA slot budget.

---

## 9. Testing strategy

Aligned with `plan-mqtt-ux-v2.md` Phase 0 harness.

**Unit (host build, `tests/unit/test_timers.c`)**

- JSON serialize → parse → re-serialize round-trip.
- Day-mask parsing: `"--TW--S"` → bits set for Tue/Wed/Sat only.
- `next_fire_unix` correctness across DST-naive boundaries (we don't do DST;
  user picks `tz_offset_min` and lives with it — documented).
- Once-mode disarms after fire.
- Window jitter deterministic given fixed seed.

**Integration (pytest, against real device or QEMU)**

- `PUT /api/timers/1` with a `time` 90 s in the future, listen on the topic,
  assert publish arrives within `[t-1s, t+2s]`.
- `arm=0` slot never fires even when its scheduled minute hits.
- `repeat=0` fires once then `arm` flips to `0` and survives a reboot.
- `topic="$SYS/x"` is rejected with 400.
- Bulk: create 16 timers with overlapping minute, assert all 16 publish
  within the same 2 s window without dropping.
- Power-loss durability: write 16 timers, `idf.py monitor`-style reset,
  assert all 16 survive.

**Playwright (UI)**

- `/timers` lists 16 rows, 15 empty by default.
- Click slot 3 → form renders with empty defaults, Arm unchecked.
- Submit invalid time `25:00` → form re-renders with inline error
  `Time must be HH:MM 24h`.
- "Save & test fire now" → after redirect, a captured WebSocket on the
  `/tester` page shows the message within 5 s.
- Visual-diff baselines under `docs/screenshots/timers/`.

**Fuzz (`tests/fuzz/test_timer_api.py`)**

- Hypothesis-driven random JSON bodies to `PUT /api/timers/1`. Broker
  must never crash; either 200 with canonicalized body, or 400.

---

## 10. Open questions

1. **DST**: Tasmota has full DST support (`Timezone 99` + `TimeStd`/`TimeDst`).
   Our broker only stores a fixed `tz_offset_min`. Acceptable for v1 (most
   home-automation users live in one TZ and accept the once-a-year manual
   bump), or do we bite off DST rules now?
2. **`cmnd/` topic namespace**: collides with anyone already running
   Tasmota devices on the same broker. Default-off behind a setting is the
   safe call — confirm we agree.
3. **Test-fire rate limit**: 1/sec/slot is generous. Tighten to 1/5 s if
   we worry about UI button mashing causing relay flapping downstream.
4. **Per-slot enable vs global disable**: Tasmota has both (`Timers 0`
   master kill). Do we expose a global toggle on `/timers`, or just rely on
   per-slot `arm`? Recommend: add a single "Pause all timers" toggle at
   top of `/timers`, persisted as `tz_offset_min`'s neighbor `timers_master`
   bool in NVS.

---

## 11. Out of scope (recorded so future-me doesn't redo the conversation)

- Sub-minute intervals — use a client-side cron job, not the broker.
- Multi-action chains — would require a rule engine; punt to a future
  "Rules" plan if there's demand.
- Sunrise/sunset astronomical modes (Civil / Nautical / Astronomical) —
  Tasmota has 4; we'd ship one (Normal) in Phase 2 and stop there.
- Per-timer auth (run timer X as MQTT user Y). The broker has a single
  internal-publish identity (`__broker`) and ACLs treat it as trusted.
- Editing the JSON blob directly from a textarea in the UI. Tempting,
  but invites footguns; the API covers programmatic use.
