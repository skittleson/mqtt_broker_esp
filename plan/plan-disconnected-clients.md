# Plan — Persistent Disconnected-Client History on `/clients`

> Status: **draft**. Targets a follow-up release after 0.7.x. Owner: TBD.
>
> Origin: user request — "I would like to see disconnected clients on
> `/clients` that persist across reboots of the device. More flash wear
> is acceptable if the trade is justified — even one write per week is
> fine if it lets the device last 10 years."

## 1. Why this matters

Today `/clients` (and `GET /api/clients`) only shows **currently
connected** MQTT sessions. The moment a sensor disconnects — flaky
WiFi, battery swap, firmware update on the sensor side — it vanishes
from the dashboard. There is no way to answer:

- "Did the kitchen humidity sensor reconnect after I reflashed it?"
- "Which client was at `192.168.8.42` last week and why is it gone?"
- "How many messages did `thermostat-01` push before it went dark?"

This makes the broker a poor **fleet-health** tool. The existing live
view is great for "is the system OK right now"; it is useless for "what
just changed". Persistence across reboots is what turns this into an
inventory / monitoring surface for the long-running home-automation and
field-deployment use cases the README calls out.

## 2. Goals & non-goals

### Goals

1. Show recently-disconnected MQTT clients on `/clients` (HTML + JSON),
   sorted by `last_seen` (most recent first).
2. Survive reboots. After a power cycle, the disconnected list is
   visible without waiting for any client to come back.
3. Stay within the project's **10-year device-life goal** (per
   `README.md` §"MQTT QoS", the goal that precluded flash-backed QoS-1
   queues).
4. Per-client cumulative counters (`published`, `total_connected_s`,
   `connect_count`) that survive both disconnects _and_ reboots — so a
   sensor's lifetime stats are meaningful, not reset by every restart.
5. User-controllable: an off switch, a flush-interval knob, and a
   manual "clear history" action in the portal.

### Non-goals

- Persisting QoS-1 in-flight queues, retained messages, subscription
  state, or session-present semantics. Those are intentionally
  ephemeral and out of scope of this plan.
- Per-message audit log. We track session-level summaries, not a stream
  of every PUBLISH.
- Flash-backed durability for _connected_ clients (live state stays in
  PSRAM as today).
- Replacing the existing TTL-based retained-message store, which has
  its own write policy.

## 3. Flash-wear budget — the central design constraint

The W25Q128 / equivalent NOR flash on these ESP32-S3 boards is rated
for **~100,000 program/erase cycles per sector**, with NVS adding a
wear-leveling layer on top. The user's "1 write per week is fine if it
buys 10 years" line implicitly sets the budget. Let's make that
explicit.

| Write cadence                 | Erases / 10 yrs | Headroom vs 100k cycles | Verdict        |
| ----------------------------- | --------------: | ----------------------: | -------------- |
| 1 / hour                      |          87,600 |                  1.14 × | Marginal       |
| 1 / 15 min                    |         350,400 |                  0.29 × | **Too hot**    |
| **1 / disconnect**¹           |        50k – 5M |                 0.02–2× | **Too hot**    |
| 1 / day                       |           3,652 |                    27 × | Comfortable    |
| **1 / hour _max_, coalesced** |          ≤8,760 |                   ~11 × | **Sweet spot** |
| 1 / week                      |             520 |                   190 × | Bulletproof    |

¹ A noisy MQTT fleet (battery sensors waking every 30 s) would burst
thousands of disconnects/hour. Write-on-disconnect is unbounded and
unacceptable.

NVS's wear-leveling spreads these erases across the partition's pages,
multiplying the budget by `pages × wear_leveling_efficiency`. Even
without that, the **coalesced ≤1/hour** policy gives ~11× margin at
10-year life. We will design to that target and treat NVS wear leveling
as bonus headroom, not a load-bearing assumption.

### Design rule: **NEVER write on a disconnect event.**

All flash writes are driven by a timer + dirty flag, not by client
lifecycle events. The in-RAM table is updated immediately; flash
catches up later.

## 4. Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│ broker_task (Core 1)                                               │
│                                                                    │
│   on client_disconnect(idx):                                       │
│     1. Copy client struct → in-RAM disconnected_history[]          │
│        (LRU; cap = HIST_MAX_CLIENTS, default 256)                  │
│     2. Set dirty_since_ms = now                                    │
│     3. Return immediately. No flash I/O.                           │
│                                                                    │
│   on client_connect(c):                                            │
│     1. Find by client_id in history → restore cumulative counters  │
│        (published_total, connect_count, total_connected_s)         │
│     2. Remove from history list (it's live again)                  │
│     3. Set dirty_since_ms = now                                    │
└────────────────────────────────────────────────────────────────────┘
                            │
                            │ shared (mutex-protected) array
                            ▼
┌────────────────────────────────────────────────────────────────────┐
│ hist_persist_task (Core 0, low prio, 4 KB stack)                   │
│                                                                    │
│   loop every HIST_FLUSH_CHECK_S (default 60 s):                    │
│     if dirty and (now - dirty_since_ms) >= HIST_FLUSH_MIN_MS       │
│        and (now - last_flush_ms)        >= HIST_FLUSH_GAP_MS:      │
│             serialize history → NVS blob "clients/hist"            │
│             clear dirty flag                                       │
│             last_flush_ms = now                                    │
│                                                                    │
│   on graceful shutdown (esp_reboot hook):                          │
│     force one final flush if dirty                                 │
└────────────────────────────────────────────────────────────────────┘
```

The two-timer policy (`FLUSH_MIN_MS` debounce + `FLUSH_GAP_MS` rate
limit) gives both burst protection and a guaranteed upper bound on
write frequency.

Default tuning:

| Knob                 | Default | Effect                                               |
| -------------------- | ------- | ---------------------------------------------------- |
| `HIST_FLUSH_MIN_MS`  | 5 min   | Debounce after the _first_ dirty event               |
| `HIST_FLUSH_GAP_MS`  | 1 hour  | **Hard** minimum spacing between consecutive flushes |
| `HIST_FLUSH_CHECK_S` | 60 s    | Persist-task wake interval                           |
| `HIST_MAX_CLIENTS`   | 256     | LRU cap on in-RAM + on-flash history                 |
| `HIST_RETAIN_DAYS`   | 30      | Drop entries older than this on flush                |

`HIST_FLUSH_GAP_MS = 1 h` is the load-bearing number for the 10-year
budget (~87.6k erases over 10 years, before NVS leveling). User can
raise this to **1 / day** or **1 / week** from `/settings` if they want
even more headroom; the values 1 h / 1 d / 1 w / off will be exposed as
a select box, not a free-form number, so there's no way to accidentally
type "1 minute".

## 5. Data model

### In-RAM struct (PSRAM)

```c
/* Total: ~120 B per entry. 256 entries = ~30 KB PSRAM. */
typedef struct {
    char     client_id[64];        /* MQTT client identifier */
    char     last_ip[16];          /* last-known IP string */
    int64_t  first_seen_epoch_s;   /* requires NTP sync, else 0 */
    int64_t  last_seen_epoch_s;    /* ditto */
    uint32_t total_published;      /* cumulative across sessions */
    uint32_t connect_count;        /* sessions observed */
    uint32_t total_connected_s;    /* cumulative on-time */
    uint8_t  flags;                /* bit0 = clean disconnect, bit1 = auth ok */
    uint8_t  _pad[3];
} client_history_entry_t;
```

NTP is already a 0.7.0 dependency, so `epoch_s` is real wall-clock
time. Pre-sync, entries are stamped with `0` and re-stamped on the
first post-sync write (the persist task notices and back-fills using
uptime delta).

### On-flash NVS blob

Single NVS key under existing `nvs` namespace (or a new `clients`
namespace) — one `nvs_set_blob` per flush. Layout:

```
header:    magic[4] = "MQHC"  /* MQTT Hist Clients   */
           version  u8        /* schema = 1          */
           count    u16        /* number of entries   */
           reserved u8
records:   count × client_history_entry_t (packed)
```

Size at 256 entries: ~30 KB blob. The current NVS partition is only
24 KB (`partitions.csv`: `nvs, ..., 0x6000`), and shares space with WiFi
credentials, MQTT auth, retain config, AP config, etc. **That's a
blocker.**

Two options, both honest:

- **Option A — repartition.** Bump `nvs` from `0x6000` → `0x10000`
  (64 KB). One-off cost on first flash of the new firmware (NVS
  partition layout change requires a full erase, which **wipes saved
  WiFi credentials**). Migration story: ship a release note flagging
  this; the device will pop back into AP-mode setup on first boot, then
  Just Work. This is the recommended path because it leaves room for
  future settings.
- **Option B — dedicated partition.** Add a new `clients_hist` data
  partition (`nvs` type, 16 KB) in `partitions.csv`. Same NVS APIs,
  isolated namespace, no migration impact on the main `nvs` partition.
  Smaller blast radius, and the wear of writing the history blob is
  fully isolated from settings flash. **Preferred for risk-averse
  rollouts**; pick this if we want to ship without a force-reflash of
  saved credentials.

The choice changes one CSV file and the `nvs_open` partition name; the
rest of the design is identical. Section 8 captures the decision as
phase 0.

## 6. UI / API surface changes

### `/clients` page

Add a third section **below** the existing "WiFi AP Clients" table:

```
Recently Disconnected (24 of 256, last 30d)              [Clear history]
┌─────────────┬─────────────┬──────────────┬─────────┬─────────────┐
│ Client ID   │ Last IP     │ Last seen    │ Sessions│ Lifetime msg│
├─────────────┼─────────────┼──────────────┼─────────┼─────────────┤
│ sensor-yard │ 192.168.8.71│ 2m ago       │   142   │    8,401    │
│ thermo-bed  │ 192.168.8.50│ 1h 12m ago   │    17   │      942    │
│ kitchen-hum │ 192.168.8.42│ 3d 4h ago    │     2   │      318    │
└─────────────┴─────────────┴──────────────┴─────────┴─────────────┘
```

- Same live-refresh mechanism as the connected table (`fetch
/api/clients` every 3 s, `visibilitychange`-aware backoff).
- Bottom-right "Clear history" button → `POST /api/clients/history/clear`
  (auth-gated). Confirms with a `confirm()` dialog. Forces an immediate
  flush so the cleared state is durable.
- "Last seen" is rendered as human relative time (`Xs ago`, `Xm ago`,
  `Xh ago`, `Xd ago`). If NTP never synced for that entry, render
  `unknown` (italic, muted).

### `GET /api/clients` (extends existing shape — additive only)

```jsonc
{
  "mqtt": [
    /* unchanged */
  ],
  "wifi_ap": [
    /* unchanged */
  ],
  "disconnected": [
    {
      "client_id": "sensor-yard",
      "last_ip": "192.168.8.71",
      "first_seen_epoch": 1737280000,
      "last_seen_epoch": 1737450000,
      "connect_count": 142,
      "total_published": 8401,
      "total_connected_s": 612340,
      "clean_disconnect": true,
    },
  ],
  "disconnected_meta": {
    "count": 24,
    "cap": 256,
    "retain_days": 30,
    "persisted": true,
    "last_flush_s": 41 /* age since last flash write */,
    "pending_flush": false /* dirty + waiting for gap     */,
    "enabled": true,
  },
}
```

`disconnected_meta` lets dashboards expose flash-write health
(`last_flush_s`, `pending_flush`) — useful for diagnosing "is my flash
budget actually being respected?" without serial monitor access.

### `/settings` — new "Client History" section

```
[ ] Track disconnected MQTT clients across reboots
    Flush interval: ( ) 1 hour   ( ) 1 day   ( ) 1 week
    Retain entries: [____30____] days  (1 – 365, 0 = until cap hit)
    Cap entries:    [____256___]       (32 – 512)
```

Standard "save = confirm + reboot" flow per 0.6.3.

### New endpoints

| Path                         | Method | Auth | Description                                         |
| ---------------------------- | ------ | ---- | --------------------------------------------------- |
| `/api/clients/history`       | GET    | yes  | Same as `disconnected` slice above, standalone      |
| `/api/clients/history/clear` | POST   | yes  | Wipe history + force flush                          |
| `/api/clients/history/flush` | POST   | yes  | Debug: force a flash write _now_ (counts vs budget) |

## 7. `$SYS` topics (free win)

While we're in here, publish two retained `$SYS` topics so MQTT clients
can subscribe to the same data without scraping HTML/JSON:

- `$SYS/broker/clients/disconnected/count` — `int`, retained, updated
  on every change (in RAM; no flash impact)
- `$SYS/broker/clients/history/last_flush_age_s` — `int`, retained,
  updated every 60 s

Optional follow-up (not in scope for v1): per-client retained topics
`$SYS/broker/clients/<id>/last_seen_epoch`. Useful for Home Assistant
template sensors, but adds a multiplier to the retained-store size.
Decide after we see real-world fleet sizes.

## 8. Phased rollout

Each phase is independently shippable and testable.

### Phase 0 — partition decision (1 doc edit)

Pick Option A or B from §5. Land the CSV change + a release note for
the credential-wipe migration (if A). **No code yet.** This phase exists
so the partition layout is settled before any code references the new
NVS namespace.

### Phase 1 — in-RAM history, no flash

- `client_history_entry_t` table in PSRAM (cap 256, LRU).
- `client_disconnect()` populates history before zeroing the slot.
- `client_connect()` looks up by `client_id`, restores cumulative
  counters, removes from history.
- `broker_get_clients_history()` API mirroring `broker_get_clients()`.
- `/api/clients` JSON gains `disconnected` + `disconnected_meta`
  (`persisted: false` for now).
- `/clients` HTML gets the new table.
- Tests in `test_broker.py`: connect, disconnect, verify history; cap
  enforcement (257 entries → oldest evicted); reconnect merges
  counters.

**Ship gate:** survives a stress test of 1k connect/disconnect cycles
with no allocation growth. No reboot persistence yet.

### Phase 2 — NVS persistence

- `hist_persist_task` on Core 0 with the two-timer policy from §4.
- `nvs_set_blob` / `nvs_get_blob` of the packed history.
- Settings UI section (track on/off, flush interval).
- Endpoints: `/api/clients/history/clear`, `/api/clients/history/flush`.
- `disconnected_meta.persisted` flips to `true`.

**Tests:**

- Connect/disconnect 10 clients, hard power-cycle the device, confirm
  all 10 visible in `/clients` within 5 s of boot.
- Write-budget test: instrument `nvs_set_blob` (count calls via a
  weak symbol or test-only counter exposed at `/api/clients/history`),
  generate 10k disconnects in 1 minute, assert flash-write count ≤ 1.
- Settings reboot loop test (already in `test_broker.py`) updated to
  cover the new fields.

**Ship gate:** flash-write counter shows ≤ 24 writes/day across a
24-hour soak with a churning fleet.

### Phase 3 — `$SYS` topics + polish

- Publish the two retained topics from §7.
- Add the `Recently Disconnected` row to the live screenshots
  (`tools/capture_clients.py`).
- README section under "Client Tracking and Monitoring".
- CHANGELOG entry.

### Phase 4 (deferred) — opportunistic improvements

- Per-client `$SYS/broker/clients/<id>/...` retained topics (size impact
  needs measurement).
- Last-Will-and-Testament aware flag (`flags.bit2 = died_with_LWT`).
- CSV download endpoint (`GET /api/clients/history.csv`) for
  spreadsheet-driven inventory.
- Configurable `HIST_FLUSH_GAP_MS` from the portal (free-form, with a
  safety floor of 5 min) instead of the three preset radio buttons.

## 9. Risks & mitigations

| Risk                                                       | Mitigation                                                           |
| ---------------------------------------------------------- | -------------------------------------------------------------------- |
| Flash wear underestimated; user sets `1 hour` + busy fleet | Hard floor `HIST_FLUSH_GAP_MS ≥ 1 h`, expose flush count at `/api`   |
| NVS partition full → silent flush failures                 | Repartition (Option A) gives 64 KB headroom; log + `$SYS` alert      |
| WiFi-credential wipe on Option A migration                 | Release note + visible AP-mode fallback handles it; tested manually  |
| `client_id` PII in flash (e.g. MAC-derived IDs)            | Document; provide `Clear history` button + a master "disable" switch |
| In-RAM history doubles PSRAM cost in pathological case     | Cap = 256, ~30 KB PSRAM — well inside the 6 MB free heap budget      |
| Reboot during flush → torn blob                            | NVS is power-fail safe at the key level; old key remains valid       |

## 10. Open questions

1. **NTP dependency.** Should we _block_ persistence until NTP is
   synced at least once, or stamp with `0` and back-fill? Current plan:
   back-fill, with a `unknown` UI fallback. Confirm with a quick
   stakeholder check.
2. **Merge semantics on `client_id` collision.** Default plan: same
   `client_id` reconnecting merges (sums) into the existing history
   entry. Alternative: keep a per-session log. Logging is richer but
   bumps storage 5–10×. Default wins for v1.
3. **Auth scoping.** `GET /api/clients` is already auth-gated when
   Basic Auth is on. Should the history slice be visible _without_ auth
   (like `/api/ping`)? Probably no — client IDs and IPs are sensitive.
4. **Option A vs B in §5.** Defaults to Option B (dedicated partition)
   in the absence of objections — lower migration blast radius.

## 11. Acceptance checklist

- [ ] `/clients` shows a `Recently Disconnected` table populated within
      5 s of a client closing its TCP socket.
- [ ] Power-cycling the device leaves the table populated on reboot
      (post-Phase 2).
- [ ] A fleet that bursts 1k disconnect events in 60 s causes **at
      most one** flash write.
- [ ] `Clear history` button empties both RAM and flash and confirms via
      a follow-up GET.
- [ ] Default config sustains the 10-year device-life goal with > 10×
      headroom on the 100k-cycle NOR endurance rating.
- [ ] `make test` (the existing 129-test suite) stays green; ≥ 5 new
      tests cover history-specific behavior.
- [ ] No regressions to `/api/ping` latency or `/clients` p95 (per the
      0.6.6 portal-latency analysis).

---

**TL;DR.** Add an in-RAM, PSRAM-resident LRU table of disconnected
clients populated on every `client_disconnect()`. A separate task
flushes it to NVS on a **strict ≥ 1-hour cadence** (configurable up to
weekly). That cadence stays inside the project's 10-year flash budget
with > 10× headroom, so the user gets persistent disconnected-client
visibility without compromising device longevity.
