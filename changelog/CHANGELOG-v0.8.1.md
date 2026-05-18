# 0.8.1 — /timers UX fixes

Patch release that closes every correctness defect found in the 0.8.0 UX
audit ([`docs/timers-ux-audit-v0.8.0.md`](../docs/timers-ux-audit-v0.8.0.md)),
plus four of the higher-impact polish items. No protocol or storage
changes — pure portal-render adjustments.

## P0 — correctness

- **Numeric timezone offset on `/timers`.** The previous build appended
  `%Z` to the local timestamp, which newlib renders as the literal string
  `"UTC"` for POSIX TZ strings like `UTC7`. That produced contradictory
  output (`local 2026-05-18 12:13:13 UTC` on a PDT device). Replaced with
  `strftime("%z")` reformatted to `(UTC-07:00)` — unambiguous regardless
  of the TZ string. Works for `PST8PDT`, `<+0530>-5:30`, `UTC0`, etc.
- **Dropped the "24h" claim on the time input.** `<input type='time'>`
  honors the browser's locale, so US Chrome / Firefox / Safari render the
  field as `05:00 PM` no matter what label the server emits. Label now
  reads simply `Time (local)`; the POST body still carries the
  unambiguous 24-hour `HH:MM` value the spec mandates.
- **Empty slot's edit form no longer pre-fills `12:00 AM`.** When a slot
  has no label / no topic / arm off / minute-of-day 0 (the cleared
  state), the time `<input>` now renders without `value=`. Browsers show
  it as `--:-- --`, forcing a conscious choice. Once any field is set,
  subsequent loads render `value=` normally.

## P1 — significant UX

- **Dedicated `Rep` column on `/timers`.** The previous build jammed
  `once` into the Time cell as `11:49 once`. Now the table has a separate
  `Rep` column with `↻` for repeating and `1×` (grey) for one-shot.
  Time column is just `HH:MM`. Mobile wraps cleanly.
- **Three-state `On` indicator** with clear visual distinction:
  - `●` green = armed
  - `◐` orange = configured but disarmed
  - `—` grey  = empty slot
  Each carries a `title=` tooltip so hover/long-press explains the state.
- **`Next fire` line under the edit-form time picker.** Renders the
  scheduler's actual `next_fire_unix` in human form (`today at 17:00
  (in 4h 35m)`, `tomorrow at 06:30`, `Sat Jun 6 at 22:00`, or `Not
  scheduled`). Uses the device's POSIX TZ — so a misconfigured zone
  surfaces *on the form* before the user discovers it at the scheduled
  moment. Empty slots show `Not scheduled`.
- **Trimmed `/timers` footer.** Removed the "Click a row to edit" hint
  (the labels are obviously links). The "Dropped fires" counter is now
  rendered only when > 0 — zero is the boring case.

## Out of scope (planned for 0.8.2)

- **Mobile card layout** for the timer list (P1 #6 in the audit). On
  390 px viewports the 7-column table still wraps the label to multiple
  lines. Card layout is shared work with `/clients` per the v2 plan.
- **Master pill instead of full-width button** (P1 #7). Pause-all is
  still a banner-style button.
- **Desktop button row + back-as-breadcrumb** (P2 #10 / #11).

## Out of scope (planned for 0.8.3)

- **`PUT` / `DELETE /api/timers/<n>`** — the audit confirmed only the
  read endpoint shipped in 0.8.0. Adding the write surface is purely
  additive; no UI changes.

## Flash impact

`mqtt_broker.bin` grew from `0x11ec20` → `0x11f100`, a delta of **1,248
bytes**. OTA slot is still 72% free. Two small static helpers
(`portal_format_local_time`, `portal_format_next_fire`) and the
restructured table account for almost all of it.

## Verified live

- Tagged after OTA-upgrading a Waveshare ESP32-S3-ETH on the bench
  (`http://192.168.22.100`), re-arming the `cmnd/tasmotas/POWER` timer,
  and re-running `tools/capture_timers.py` to confirm every audit
  finding renders as expected on desktop and mobile.
- Audit doc updated in place to mark P0 #1/#2/#3 + P1 #4/#5/#8/#9 as
  shipped.
