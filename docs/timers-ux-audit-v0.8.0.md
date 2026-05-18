# /timers UX audit — v0.8.0

Date: 2026-05-18 · Device: `http://192.168.22.100` (firmware 0.8.0)

Evidence: `docs/screenshots/timers/{list,edit_populated,edit_empty,edit_saved}_{desktop,mobile}.png`
Regenerate: `PORTAL_URL=… PORTAL_AUTH=user:pass python3 tools/capture_timers.py`

What worked well first, then defects ordered by severity.

## What's right

- Single-page form covers the whole Tasmota timer model in one viewport on
  desktop. No tabbed sub-views, no JS required to fill it out.
- **`Saved.` green banner** at the top of `edit_saved_*` is honest and
  unambiguous (`edit_saved_desktop.png`).
- **Day-mask presets** (`Weekdays · Weekends · Every day · Clear`) work
  client-side, no extra HTTP round-trip.
- **`Clear slot` requires `confirm()`** before wiping data.
- **Empty-slot rows** are clearly distinguished in grey with `(empty —
configure)` links instead of looking armed-but-misconfigured.
- **Test fire and Save are visually distinct buttons** (different colors,
  separate `<form>`s — clicking Test fire cannot accidentally save).

## P0 — correctness defects

### 1. Timezone label says "UTC" when device is on `UTC7`

`list_desktop.png`, header line:

```
0 of 16 armed · master enabled · local 2026-05-18 12:13:13 UTC
```

The device's POSIX TZ is `UTC7` (UTC-7 / PDT-equivalent), so the wall
clock `12:13:13` IS the user's local time — but the `%Z` abbreviation
that newlib emits for `UTC7` is the literal string `"UTC"`. The display
contradicts itself ("local" + "UTC" suffix on a non-UTC time).

**Fix:** drop `%Z` from the strftime format and append the numeric
offset instead: `local 2026-05-18 12:13:13 (UTC-07:00)`. Numeric offset
is unambiguous and survives any POSIX TZ string the user types
(including `UTC0`, `PST8PDT,…`, `<+0530>-5:30`, etc.).

### 2. Time input renders as 12-hour AM/PM despite "24h" label

`edit_populated_*`, `edit_empty_*`:

```
Time (local, 24h)
[ 11 : 49 AM ⏰ ]
```

The `<input type='time'>` honors the browser's locale, so US users get
AM/PM regardless of what the form label claims. The form will still
POST a 24-hour value (`HH:MM`), but the visual mismatch is a small lie
and removes the "24h" assurance the label promises.

**Fix:** drop "24h" from the label since we can't force it without JS.
Replacement: `Time (local)` + a small hint `Device interprets in your
configured time zone (see Settings)`.

### 3. Empty-slot edit form shows `12:00 AM` as the default time

`edit_empty_desktop.png`: blank slot 2 renders with `[12:00 AM]` (i.e.
`minute_of_day = 0`) as if it had been configured to fire at midnight.
Combined with the unchecked Arm / Repeat / days, it's harmless — it
won't fire — but it visually misleads a first-time user into thinking
there's already a time set.

**Fix:** server-render the time `<input>` without a `value=` when the
slot is empty (`s->minute_of_day == 0 && !s->arm && s->topic[0] == 0`).
Browsers then show the field as empty / placeholder, and the user picks
the time consciously. The server-side default still applies on submit
(empty string → bail out with a validation error so we never write a
fictitious 00:00 schedule).

## P1 — significant UX issues

### 4. "Time" column conflates time and repeat state

`list_desktop.png`, row 1:

```
Time
11:49 once
```

`once` is metadata jammed into the time cell. On mobile it forces an
ugly wrap. A reader scanning the column doesn't immediately know
whether `once` is a suffix, a separate field, or a typo.

**Fix:** add a one-character `R` (repeat) / `1` (one-shot) badge in a
dedicated column, or use icons (♺ vs `1×`). Frees the Time column for
just `HH:MM`.

### 5. List view: armed-vs-empty distinction too subtle

`list_*`. Armed = `●` solid green, unarmed-but-configured = `○`,
empty-slot = `—`. The `○` (slot 1, configured but `arm=false`) and the
`—` (empty) sit only one row apart and the difference is one character
and one weak color shift.

**Fix:** Use three distinct visual states:

- empty slot → grey `—`, label `(empty)`
- configured but disarmed → orange `◐`, label visible normally
- armed → green `●`, label visible normally

Plus a tooltip on hover (`title=` attribute) explaining each state.

### 6. Mobile: label column wraps "All lights ON" to three lines

`list_mobile.png`, row 1:

```
1 | ○ | All
       lights
       ON
```

390 px wide × 6 columns is too many. Label, Time, Days, Topic all
compete for the remaining ~280 px after `#` and `On`.

**Fix:** at viewport < 600 px, switch the table to **card layout** —
one stacked block per slot:

```
┌─ Timer 1 ──────────── ○ disarmed ─┐
│ All lights ON                     │
│ 11:49 · SMTWTFS · once            │
│ cmnd/tasmotas/POWER → ON          │
│ [Edit] [Test fire]                │
└───────────────────────────────────┘
```

This is a recurring need on this portal (also affects `/clients` per
the v2 plan §7.3); shared CSS only.

### 7. "Pause all timers" button is full-width and weighs the page

`list_*`. The master pause control is rendered as a big grey button
spanning the entire fieldset. Visually it dominates a page that's
mostly meant for listing schedules.

**Fix:** replace with a small inline toggle in the header line:

```
0 of 16 armed · master [enabled]   ← clickable pill
```

Click toggles enable/disable, posts to `/timers/master`. Frees vertical
space and removes a button that's only used twice in the device's
lifetime.

### 8. "Click a row to edit. Dropped fires: 0" footer

`list_*`, bottom of fieldset. Two unrelated bits glued together. The
"Click a row to edit" hint is unnecessary — the labels are obviously
links (blue, underlined). The "Dropped fires" counter is useful but
buried.

**Fix:** delete the click-hint. Move "Dropped fires" into a small
diagnostic line under the header timestamp, formatted as `0 dropped
fires since boot` — and only when > 0 (zero is the boring case).

### 9. Edit page never shows `next_fire_unix` to the user

`edit_populated_*`. The API has `next_fire_unix` per slot; the form
doesn't. Users can't easily verify "is my 17:00 schedule going to fire
at the right time in my actual timezone?" without leaving the form.

**Fix:** under the time picker, render a one-liner:

```
Time (local)
[17:00]
Next fire: today at 17:00 (in 5h 12m)
```

Server-side, formatted using the same TZ the scheduler uses. Catches
TZ misconfiguration _in context_ — before the user discovers it the
hard way at dusk.

## P2 — polish

### 10. Action buttons stacked full-width on desktop wastes space

`edit_populated_desktop.png`, bottom: three full-width buttons (Save
→ Test fire → Clear) take ~150 px vertical. On a 1024 px desktop these
should sit on one row.

**Fix:** at ≥ 600 px viewport, switch to `display:flex; gap:8px` so
the three buttons share a row. Mobile keeps stacked.

### 11. "Back" button at the very bottom

`edit_*`. Going back to the list is a primary action but lives below
the destructive `Clear slot` button. Easy to misclick on mobile.

**Fix:** move `Back` to the top of the page (a `← Back to timers`
breadcrumb) and keep the buttons grouped at the bottom for the form's
own actions.

### 12. Empty-slot edit page shows defaults that don't actually save

`edit_empty_*`: the form pre-fills `time=12:00 AM`, `Sun..Sat`
unchecked, default QoS 0, etc. A user could click Save without
filling in anything and we'd 400 them ("topic required when armed").
But the topic placeholder `home/lights/cmd` is grey — they might think
it's already configured.

**Fix:** when slot is empty and arm not yet checked, place a soft hint
above the form: `Empty slot — fill in the action below, check Arm to
enable.`

### 13. `mqtt_broker 0.8.0` footer reads like a typo

The product is `MQTT Broker` (header) but the footer renders
`mqtt_broker 0.8.0`. Inconsistency.

**Fix:** capitalize to match: `MQTT Broker 0.8.0`. (`FW_NAME` is
`"mqtt_broker"`; either change the constant or just format the footer
differently.)

### 14. No "duplicate slot" action

If you have 5 lights that need the same schedule with different topics,
you re-fill the whole form 5 times. Tasmota itself doesn't solve this
either, but it would be a low-cost addition.

**Fix:** `[Copy to slot…▾]` dropdown on edit page, posts to
`/timers/copy?from=1&to=3`. Out of scope for this audit; tracked.

### 15. No `/api/timers` write surface yet

Plan §5 promised `PUT /api/timers/<n>` and `DELETE /api/timers/<n>`.
Shipped 0.8.0 only has the read endpoint plus form POSTs. For
headless automation (Home Assistant, Ansible) this matters.

**Fix:** ship in 0.8.1 as a pure additive change — no UI implications.
Reuse `timers_set()` / `timers_clear()` directly.

## Suggested sequencing

| Phase | Items                                                    | Effort | Risk                                   |
| ----: | -------------------------------------------------------- | ------ | -------------------------------------- |
|    1a | P0: #1, #2, #3 (clock label + time fields)               | 0.5 d  | low                                    |
|    1b | P1: #4, #5, #8, #9 (list-view fixes + next-fire on edit) | 1 d    | low                                    |
|     2 | P1: #6 (mobile card layout)                              | 0.5 d  | low (shared CSS, used by /clients too) |
|     3 | P1: #7 (master pill)                                     | 0.5 d  | low                                    |
|     4 | P2: #10, #11, #12, #13                                   | 0.5 d  | low                                    |
|     5 | P2: #15 (`PUT`/`DELETE` API)                             | 0.5 d  | low                                    |
|     6 | P2: #14 (duplicate slot)                                 | 0.5 d  | low                                    |

Total: ~4 dev-days, all additive, no flash penalty > 2 KB.

Phase 1 alone closes every actual correctness issue and would land in a
0.8.1 patch release.
