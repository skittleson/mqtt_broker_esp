# Plan: Echo Detection (P2)

The broker detects echo loops — when a topic receives ≥ N publishes within M seconds, it flags the topic.

## Goals

- Detect per-topic echo loops (≥ 3 publishes in 60s window).
- Expose detected topics via HTTP API and dashboard widget.
- Configurable via NVS (toggle, sensitivity).

## Non-goals

- Auto-subscribe the broker to flagged topics.
- Auto-block/suspend echo publishers.
- Echo loop source identification (which client published first).
- Bridge-mode echo detection (separate feature).
- Per-client echo detection.

## Data model

### NVS keys (`mqtt_cfg` namespace)

| Key          | Type   | Default | Description                     |
| ------------ | ------ | ------- | ------------------------------- |
| `echo_en`    | u8     | 1       | Echo detection enabled          |
| `echo_count` | u16    | 3       | Min publishes to trigger        |
| `echo_window`| u16    | 60      | Window in seconds               |

### In-memory state (broker_task only)

```c
#define ECHO_MAX_TOPICS   32

typedef struct {
    char     topic[MQTT_MAX_TOPIC_LEN + 1];
    uint16_t topic_len;
    uint16_t count;          /* publishes in current window */
    int64_t  window_start;   /* when the current window started (ms) */
    int64_t  detected_at;    /* first detection timestamp (ms), 0 if not detected */
} echo_entry_t;

typedef struct {
    echo_entry_t entries[ECHO_MAX_TOPICS];
    int          count;
    bool         enabled;
    uint16_t     min_count;
    uint16_t     window_sec;
} echo_state_t;
```

Sliding window: on each PUBLISH, increment the counter for matching
topics. If the counter hits the threshold, mark `detected_at`. A new
publish on the same topic that falls outside the window resets the
counter (old entry expires, new window starts).

### HTTP API

| Path                | Method | Auth  | Description                             |
| ------------------- | ------ | ----- | --------------------------------------- |
| `/api/echo-detected`| GET    | gated | JSON array of detected topics + counts  |
| `/api/echo-reset`   | POST   | gated | Clear all detection state               |

#### `GET /api/echo-detected` response

```json
{
  "detected": [
    {
      "topic": "home/sensor/temp",
      "count": 5,
      "detected_at": 1779129423
    }
  ],
  "total": 1
}
```

#### `POST /api/echo-reset` response

```json
{ "reset": true }
```

### Dashboard widget

A small card on the dashboard (near retained message counter) showing
echo detection status: `echo: N detected` or `echo: clean`. Clicking
the card calls `/api/echo-reset`.

## Architecture

### New files

- `main/echo_detect.c` — detection logic, state, config, API
- `main/echo_detect.h` — public API header

### Integration points

- `mqtt_broker.c` calls `echo_track(topic, topic_len)` from
  `handle_publish_internal()` after fanout (same pattern as
  `retain_store()`).
- `portal.c` calls `echo_get_detected()` for the dashboard widget and
  the `/api/echo-detected` endpoint.
- `/save-settings` handler reads/writes NVS keys for echo config.
- `main.c` calls `echo_init()` after `ntp_init()` (no ordering
  dependency, but after broker_start so the config is available).

## Phasing

### Phase 1: Detection engine (P0)

- `echo_detect.c`/`.h` with detection logic and API.
- Integration into broker_task's publish path.
- `GET /api/echo-detected` endpoint.
- ~500 bytes flash delta.

### Phase 2: Config + dashboard (P1)

- NVS config persistence.
- Dashboard widget.
- ~200 bytes flash delta.

### Phase 3: Reset API + tests (P1)

- `POST /api/echo-reset` endpoint.
- Test coverage in `test_broker.py`.
- ~200 bytes flash delta.

## Verification

1. `idf.py build` clean (no new warnings).
2. OTA deploy, publish 4 messages to the same topic within 60s, verify
   `/api/echo-detected` returns the topic.
3. Verify window expiry: wait 60s + publish 1 more, verify it resets.
4. Verify NVS config: change `echo_count` to 5, verify detection
   requires 5 publishes.
5. `make test` green (echo detection doesn't break existing tests).

## Flash budget

| Phase  | Estimated delta |
| ------ | --------------- |
| P0     | ~500 bytes      |
| P1     | ~200 bytes      |
| P2     | ~200 bytes      |
| Total  | ~900 bytes      |
