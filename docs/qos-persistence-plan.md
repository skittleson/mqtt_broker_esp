# Plan: QoS 1/2 + Persistent Sessions

Status: **planned, not implemented.** Today the broker is QoS 0 only and all
session state is in RAM. This document scopes what it would take to add
guaranteed delivery and session persistence, so a future PR isn't a rewrite.

## Motivation

QoS 0 ("fire-and-forget") is fine for the current Tasmota sensor fleet:

- Telemetry is periodic; a dropped message is replaced 30 s later.
- Commands are usually idempotent (`POWER ON`).
- Loss only matters if the device or broker reboots mid-message.

When we _would_ need QoS 1/2 + persistence:

- Door/window/motion events (single message; loss = missed event).
- Energy meter pulse counts (loss = bad math).
- Devices that subscribe with `clean_session=false` and expect queued messages
  after a reboot.

## Scope summary

| Capability             | QoS 0 (today) | QoS 1 (planned)            | QoS 2 (planned, optional) |
| ---------------------- | ------------- | -------------------------- | ------------------------- |
| Inbound PUBLISH ack    | none          | PUBACK                     | PUBREC → PUBREL → PUBCOMP |
| Outbound PUBLISH retry | none          | resend until PUBACK        | 2-step handshake          |
| Packet identifiers     | not tracked   | per-client 16-bit pool     | per-client 16-bit pool    |
| In-flight cap          | n/a           | 20 / client (proposed)     | 20 / client               |
| Persistent session     | no            | yes (clean=0), PSRAM only  | yes, PSRAM only           |
| Survives reboot        | no            | **no** (PSRAM is volatile) | **no**                    |

**Recommendation:** ship QoS 1 + persistent sessions first. Skip QoS 2 unless a
real device demands it — almost no home-automation device uses it.

## Protocol pieces missing today

Add to `mqtt_parser.[ch]`:

- `MQTT_PUBACK` (type 4, 2-byte packet id)
- `MQTT_PUBREC` (type 5)
- `MQTT_PUBREL` (type 6, flags 0x02)
- `MQTT_PUBCOMP` (type 7)
- PUBLISH header: respect the QoS bits (1, 2) and DUP flag; emit packet id when QoS > 0.
- CONNACK session-present bit when resuming a persistent session.

## Per-client state additions (`broker_client_t`)

```c
typedef struct {
    uint16_t  packet_id;     // outbound packet id from PUBLISH
    uint8_t   qos;
    uint8_t   state;         // WAIT_PUBACK | WAIT_PUBREC | WAIT_PUBCOMP
    uint32_t  next_retry_ms;
    uint8_t   retries;
    uint16_t  topic_len;
    uint16_t  payload_len;
    uint8_t  *topic;         // PSRAM
    uint8_t  *payload;       // PSRAM (capped to retain max, 64 KB)
} inflight_t;

typedef struct {
    bool clean_session;
    uint16_t next_packet_id;
    inflight_t  out_inflight[MAX_INFLIGHT_PER_CLIENT];   // 20
    uint16_t    in_unacked[MAX_INFLIGHT_PER_CLIENT];     // packet ids we PUBREC'd
    // Queue of messages stored while client offline (clean=0):
    inflight_t *offline_queue;   // ring buffer head/tail
    uint16_t    offline_head;
    uint16_t    offline_tail;
} session_t;
```

Memory budget at 100 clients × 20 in-flight × ~80 B header + 1 KB avg payload:
~2 MB worst case. Cap total in-flight + offline-queue bytes at **2 MB** with
LRU eviction so we don't push retained messages out of PSRAM.

## Retry / timing rules

- Initial PUBACK timeout: **15 s**, then exponential backoff (15, 30, 60).
- Resend with **DUP=1** per spec [MQTT-3.3.1-1].
- Drop the in-flight message after **5 retries** and log; do NOT disconnect the
  client (other implementations vary; least-surprising choice).
- Wake the broker `select()` loop with a short timeout (already ~250 ms) and
  walk in-flight tables on each tick. No new task.

## Persistence (clean_session=false)

**PSRAM only — no flash writes.** A 10-year-life device cannot afford an MQTT
message queue writing to flash. NAND/NOR flash endurance is finite (typical
ESP32 flash: ~100k erase cycles per sector), and a queue that writes on every
offline message would chew through that budget in months under load. PSRAM
persistence covers the common case (a client briefly disconnects and reconnects
minutes later); broker reboots are rare and clients recover via standard MQTT
reconnect logic.

Storage: one tier only.

1. **PSRAM:** active in-flight tables + offline-queue messages for clients
   currently disconnected with `clean=0`. Lost on broker reboot.

Behaviour on broker reboot: all sessions evaporate. Reconnecting clients with
`clean=0` will get `session present=0` in CONNACK, which per MQTT 3.1.1
[MQTT-3.2.2-2] tells them to re-subscribe — the same recovery path as today.

Caps:

- 32 persistent sessions max (covers our 100-device fleet's likely subset).
- 64 queued messages / session.
- 16 KB total / session.
- LRU evict oldest session if full.

## Web portal additions

- `/clients` columns: `in-flight`, `queued (offline)`, `clean`.
- `/api/clients` JSON gains `inflight`, `queued`, `clean_session` fields.
- `/settings` → new section "Persistence":
  - Enable QoS 1 (default on once shipped)
  - Enable QoS 2 (default off)
  - Max persistent sessions (default 32)
  - Max queued per session (default 64)

## Test plan (extend `test_broker.py`)

New sections:

16. **QoS 1 round-trip** — subscriber gets message, broker sees PUBACK,
    in-flight count returns to 0.
17. **QoS 1 retry** — drop PUBACK, verify resend with DUP=1, then ACK,
    no duplicate delivery to subscriber.
18. **QoS 2 four-step** — full PUBREC/PUBREL/PUBCOMP handshake (if enabled).
19. **Persistent session resume** — client connects clean=0, subscribes,
    disconnects; broker queues N messages; client reconnects, receives all
    queued in order, gets `session present=1` in CONNACK.
20. **Session lost on broker reboot** — reboot the ESP32 between disconnect
    and reconnect; reconnecting client must receive `session present=0`
    (intentional behaviour; no flash persistence).
21. **Offline queue cap** — overflow drops oldest, not newest.
22. **Mixed-QoS subscriber** — subscribe at QoS 1, publisher at QoS 0:
    delivered at min(pub, sub) = 0.

Stress test additions:

- 50 clients with clean=0, 10 in-flight each, sustained for 10 min.
- Verify no flash writes occur during sustained QoS 1 traffic (read NVS write
  counter before/after — must be unchanged).

## Backward compatibility

- All QoS 0 traffic keeps working byte-for-byte.
- A QoS 1 PUBLISH from a client to a QoS 0 subscriber is still delivered at
  QoS 0 (per spec; we ack the publisher, fan out as QoS 0).
- Existing Tasmota fleet uses QoS 0 + clean=1 → unaffected.

## Rollout phases

| Phase                                     | Effort | Risk      | Notes                               |
| ----------------------------------------- | ------ | --------- | ----------------------------------- |
| **0. This doc**                           | —      | —         | done                                |
| **1. Parser support for ACK frames**      | S      | low       | pure additions, no behaviour change |
| **2. QoS 1 inbound (clients → broker)**   | M      | low       | broker ACKs; fan-out stays QoS 0    |
| **3. QoS 1 outbound (broker → clients)**  | M      | medium    | in-flight tables + retries          |
| **4. PSRAM-resident persistent sessions** | M      | medium    | survives client disconnects only    |
| **5. QoS 2 (optional)**                   | M      | low value | only if a device demands it         |

## Decision log

- **Single broker, no clustering** — out of scope; this is one MCU.
- **No bridge to upstream broker** — would solve persistence "for free" via a
  cloud broker, but defeats the whole "no cloud" thesis of the project.
- **No QoS 2 in v1** — almost no real-world client uses it.
- **No flash-backed persistence** — message-queue flash writes would shorten
  the device's lifespan unacceptably (we're targeting 10+ years of always-on
  operation). PSRAM-only persistence covers the 99% case (client reconnect);
  the 1% case (broker reboot) is rare and standard MQTT reconnect logic
  recovers cleanly.

## Open questions

- Do we want a _session expiry_ (MQTT 5 concept) backported, so abandoned
  clean=0 sessions don't pile up forever? Suggest: 7-day default, configurable.
- Should the LED indicate degraded state (e.g., in-flight backlog > 50%)? Maybe
  a slow amber pulse.
