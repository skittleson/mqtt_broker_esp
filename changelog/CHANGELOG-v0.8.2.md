# 0.8.2 — Timezone preset dropdown + /timers polish + JSON write API

Headline: **the Timezone field on `/settings` is now a dropdown of ~40
common IANA-derived zones**, so you no longer have to look up POSIX TZ
strings to get the right DST behaviour. Picking `US — Pacific (PT, DST)`
writes `PST8PDT,M3.2.0,M11.1.0` into the text input that persists. The
text input remains the source of truth — exotic zones (`<+0530>-5:30`
for India, custom rules, etc.) still hand-type cleanly.

Plus closes the remaining 0.8.0 audit backlog and adds the write half of
the JSON API.

## New: Timezone preset dropdown

- `main/tz_presets.{c,h}` — curated table of 40 POSIX TZ strings, grouped
  by region (North America, Europe, Asia/Pacific, South America, Africa,
  fixed offsets). Sourced from IANA tzdata 2025b.
- `<select>` on `/settings → Time (NTP)` populated from the table. The
  device's current TZ is pre-selected if it matches a preset; otherwise
  the first ("— Pick a preset…") row stays selected.
- Tiny inline JS handler copies the chosen value into the underlying
  POSIX `<input>`. Falls back gracefully without JS (the dropdown is
  inert but the text input still works).
- Helper line under the field: `POSIX TZ string. Picking a preset above
fills this field. Examples: UTC0, PST8PDT,M3.2.0,M11.1.0, <+0530>-5:30
(India).`

**This is the 10-year-lifetime fix the plan was designed around.** When
a government changes DST rules, an operator can either pick the updated
preset from a newer firmware _or_ hand-type the new POSIX string into
the same field with no firmware update at all. Rules live in
user-editable NVS, not code.

## /timers polish (closes audit P1 #6, #7, P2 #10, #11)

- **Mobile card layout** below 600 px viewport. Each slot is a stacked
  block with column-labelled fields (`On: ●`, `Label: …`, `Time: 17:00`,
  `Rep: ↻`, `Days: SMTWTFS`, `Topic: cmnd/…`). Big blue slot number
  makes scanning easy. Pure CSS media query — no JS. The same
  `data-label`-driven pattern can be reused on `/clients` later.
- **Master pause is now an inline pill** in the header line (`1 of 16
armed · [master: enabled] · local …`), not a full-width button.
  Two-color: green `enabled` / red `paused`. Click toggles the
  state via the existing `/timers/master` form.
- **Save / Test fire / Clear slot buttons share a flex row** on
  desktop ≥ 600 px. Below that, they stack full-width as before. The
  Save button lives outside the input form but submits it via HTML5
  `form='timer-save'` association (universally supported since 2014).

## JSON API completion (closes audit P2 #15)

- `PUT /api/timers/<n>` — JSON body is one slot object. Accepts both
  the compact wire keys (`a`, `r`, `rt`, `q`, `w`, `hm`, `d`, `tp`,
  `pl`, `l`) and the long-form (`arm`, `repeat`, `retain`, `qos`,
  `window`, `time`, `days`, `topic`, `payload`, `label`). Validates
  via `timers_set()`; returns `{saved, n, next_fire_unix}` on success
  or `{error: "..."}` with HTTP 400 + human-readable message on
  validation failure.
- `DELETE /api/timers/<n>` — wipes the slot via `timers_clear()`.
  Returns `{cleared: true, n}`.
- Both require CSRF (`X-CSRF-Token` header). Slot out of range → 400
  with `{error: "slot out of range (1..16)"}`.
- New public helper `timers_parse_slot_json()` exposes the JSON parser
  that the NVS load path already used internally.
- HTTP parser now recognizes `PUT` and `DELETE` methods alongside `GET`
  and `POST`. Body parsing extended for `PUT` too (existing 512-byte
  body buffer accommodates 16 slots × full-size payloads).

## Verification

- All five API behaviours smoke-tested live (`PUT` create, `GET`
  round-trip, `DELETE`, validation error, missing-CSRF rejection).
- Re-captured `docs/screenshots/timers/` after OTA. Master pill renders
  correctly inline on desktop (after a `!important` override of the
  portal-wide `button{width:100%}` default — first build had it
  full-width).
- Existing 5pm `cmnd/tasmotas/POWER=ON` schedule survived the upgrade
  and still shows `Next fire: today at 17:00 (in 4h)` on the edit page.

## Flash impact

`mqtt_broker.bin` grew from `0x11f100` (0.8.1) → `0x120520`, a delta of
**5,152 bytes** (~5 KB). Breakdown:

- TZ presets table + handler scaffolding: ~2 KB
- `PUT`/`DELETE /api/timers/<n>` route + `timers_parse_slot_json`: ~1.5 KB
- Card-layout CSS, master pill CSS, flex row CSS: ~1 KB
- New HTTP method strings (`PUT`/`DELETE` in parser and access log): minimal

OTA slot still 72% free (~3 MB headroom).

## Backlog after this release

- Mobile card layout pattern for `/clients` (audit-v2 §7.3) — shared CSS
  ready, just needs to be applied
- `?saved=` toast unification across the portal (audit-v2 §2.6)
- Sunrise/Sunset modes on timers (plan §2.0, needs lat/lon settings)
- `cmnd/<host>/Timer<n>` Tasmota MQTT bridge (plan §1.3)
- Auto-generate `tz_presets.c` from IANA tzdata via
  `tools/gen_tz_presets.py` in CI, instead of hand-curation
