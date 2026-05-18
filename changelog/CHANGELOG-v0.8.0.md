# 0.8.0 — Scheduled MQTT publishes ("Timers")

Tasmota-style wall-clock scheduling: 16 slots, each fires a configured
MQTT publish at a set local time on selected days of the week.

## What's new

- **New page `/timers`** — list of all 16 slots with armed indicator,
  label, time, days mask, topic. Click any row to edit. Master "pause all"
  toggle at the top.
- **New page `/timers/edit?n=<1..16>`** — per-slot form with:
  - Arm / Repeat checkboxes
  - Time picker (HH:MM, 24h local)
  - Window (±0–15 min random jitter, to de-thunder fleets)
  - Days picker (7 checkboxes + Weekdays / Weekends / Every day / Clear presets)
  - Publish topic, payload (≤ 256 B UTF-8), QoS 0/1, Retain
  - "Test fire now" and "Clear slot" buttons
- **New JSON endpoint `GET /api/timers`** — schema-stable list including
  `next_fire_unix` per slot (computed via `localtime_r`/`mktime`, so DST
  is honoured automatically).
- **New broker API `broker_publish_local()`** — thread-safe injection of
  publishes from non-client sources. Reuses the existing tester publish
  queue with an added QoS field; routes through normal retained-storage
  and fanout paths.
- **New module `main/timers.c`** — 1 Hz scheduler task. Loads slots from
  NVS `"mqtt_cfg"/"timers"` (single compact JSON blob, schema `v=1`).
  Persists once-mode disarm after fire. Per-slot last-fire-UTC-minute
  cache provides DST fall-back dedup and intra-minute idempotency.

## DST and 10-year lifetime

The scheduler resolves local time through newlib's `localtime_r()` +
`mktime()`, so the POSIX TZ string in NVS `"ntp"/"tz"` drives DST
behaviour. That setting is **user-editable** on `/settings`, which means
when governments change DST rules (Brazil 2019, Mexico 2022, …) the
device can be corrected via the portal without a firmware reflash — the
critical property for a 10-year deployment.

Transition behaviour:

- **Spring forward** (`02:30` doesn't exist on the transition day) —
  `mktime` returns -1, the slot is skipped for that day only.
- **Fall back** (`01:30` happens twice) — the per-slot last-fire-UTC-min
  cache ensures the slot fires exactly once at the first occurrence.

Other longevity work:

- Year 2038: ESP-IDF newlib uses 64-bit `time_t`. Safe.
- NVS wear: one write per timer save / once-mode auto-disarm. With 16
  slots and aggressive use, ~5K writes/year — NVS wear-levelling gives
  centuries of headroom.
- SNTP outage: scheduler refuses to fire until `time(NULL) >= 2023-01-01`.
  Visible on `/timers` as a red banner. No battery-backed RTC on
  ESP32-S3, so this is a documented limitation.

## Wire format (NVS blob)

```json
{"v":1,"me":1,"t":[
  {"a":1,"r":1,"rt":0,"q":0,"w":0,"hm":420,"d":"-MTWTF-",
   "tp":"home/lt/cmd","pl":"ON","l":"morning lights"},
  {}, ...
]}
```

Compact keys keep 16 fully-populated slots well under the 4 KB NVS soft
limit. Long-form keys (`arm`, `repeat`, `time`, …) are also accepted on
parse for hand-edits but never emitted.

## Validation rules

- `time` ∈ 00:00..23:59 (HH:MM 24h)
- `window` ∈ 0..15 (minutes)
- `days` mask — at least one day selected when armed
- `topic` — non-empty, ≤ 128 bytes, no `+`/`#` wildcards, no leading `$`
  (reserved by MQTT 3.1.1 §4.7.2), no control chars
- `payload` ≤ 256 bytes UTF-8
- `qos` ∈ {0, 1}
- `label` ≤ 24 bytes UTF-8

Validation runs in `timers_set()` at save time and in `timers_validate()`
before persisting; either rejects the change with a human-readable error
shown on the edit page.

## Flash impact

Binary: `0x11ec20` bytes (1.17 MB), **72% free in OTA slot**. Headroom
unchanged for OTA upgrades. ~15 KB net code added.

## Known gaps (planned for follow-up)

- No `cmnd/<host>/Timer<n>` MQTT command bridge yet — config is portal /
  JSON API only. See `plan-scheduled-publishes.md` §1.3.
- No sunrise/sunset modes — needs lat/lon config first. Plan §2.0.
- TZ preset dropdown on `/settings` not added yet — users still hand-type
  POSIX TZ strings (`PST8PDT,M3.2.0,M11.1.0` style). Plan §1.4 polish.

See [`plan-scheduled-publishes.md`](../plan-scheduled-publishes.md) for
the full design.
