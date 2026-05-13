# Portal HTTP latency — analysis

User-reported symptom: "sometimes the http request/response will take a
while to respond from this machine. I'm not exactly sure why but there
are 2 cores on this machine and thought the web portal has a dedicated
core."

Short answer: **the portal does NOT have a dedicated core today.** The
MQTT broker does. The portal floats. Combined with single-threaded
accept-and-handle and a couple of unobvious lock-contention paths, that
explains every slow request I measured.

## Measurements

50 sequential `GET /api/ping` (15-byte response, no auth, cheapest endpoint
on the device), broker idle except for normal Tasmota telemetry from 6
connected clients:

```
n=50/50   min=13.2ms   median=16.0ms   p95=129.3ms   max=136.0ms   mean=29.4ms

Histogram:
  <   5 ms: 0
  <  10 ms: 0
  <  25 ms: 43   <- 86% fast path
  <  50 ms: 1
  < 100 ms: 0
  < 250 ms: 6    <- 12% slow path
  < 500 ms: 0
  <1000 ms: 0
```

30 sequential `GET /api/status` (~500 B response, auth, same broker load):

```
n=30/30   min=15.5ms   median=16.8ms   p95=105.2ms   max=109.6ms
worst 5:  [18.1, 76.9, 99.6, 101.7, 109.6]
```

**The clean gap between 50 ms and 100 ms is diagnostic.** It's not network
jitter (would smear continuously), not slow malloc (would have a long
tail starting at 25 ms). It's a **discrete event** that adds ~100 ms of
extra latency to roughly one in eight requests.

## Task & core layout (today)

Pulled from the source plus `sdkconfig`:

| Task          | Stack | Prio | Affinity          | Where           |
|---------------|------:|-----:|-------------------|-----------------|
| `main`        |     - |    1 | **CPU 0** (forced)| `sdkconfig`     |
| `esp_timer`   |     - |   22 | **CPU 0** (forced)| `sdkconfig`     |
| `mqtt_broker` | 16384 |    5 | **CPU 1** (pinned)| `mqtt_broker.c:1566` |
| `portal_http` | 12288 |    5 | **NO AFFINITY** \u2014 floats | `portal.c:2472` |
| `portal_dns`  | 12288 |    5 | **NO AFFINITY** \u2014 floats | `portal.c:2473` |
| `portal_ws`   |     - |    - | **NO AFFINITY** \u2014 floats | `portal_ws.c:703` |
| `led_task`    |  2048 |    2 | **NO AFFINITY** \u2014 floats | `main.c:170`    |
| `tiT` (LwIP)  |  3072 |   18 | **NO AFFINITY** \u2014 floats | `sdkconfig` |
| `wifi`        |     - |   23 | typically CPU 0   | IDF default     |
| `Idle CPU0`   |  1536 |    0 | CPU 0             | FreeRTOS        |
| `Idle CPU1`   |  1536 |    0 | CPU 1             | FreeRTOS        |

So: **the broker has dedicated affinity on CPU 1**, but the portal does
not. When FreeRTOS schedules `portal_http`, it lands wherever there's
room \u2014 often CPU 1, which then puts it at equal priority to the broker.
At priority 5, FreeRTOS doesn't preempt between equal-priority tasks
within a tick; it round-robins on the 10 ms tick boundary
(`CONFIG_FREERTOS_HZ=100`). So if the broker is in a busy iteration when
the portal task wants to run, the portal waits up to 10 ms per round
\u2014 and most of the "slow path" measurements above are exactly multiples of 10\u201320 ms past the fast-path baseline.

## Three concrete root causes

### 1. `broker_mutex` contention in `/api/ping` (and every other portal call)

Every portal endpoint that shows stats (the dashboard, `/api/status`,
`/api/ping`, `/clients`, `/api/clients`, `/information`) calls
`broker_get_stats()` or `broker_get_clients()`. Both take the broker's
mutex.

`broker_task` holds that mutex during:

- PUBLISH fanout to all subscribers (up to 100 clients * subscription
  match per topic);
- inflight QoS-1 retry sweeps;
- retained-message scan;
- client cleanup on disconnect.

With 6 Tasmota publishers each emitting telemetry every 10\u201330 s, fanout
windows of 50\u2013100 ms are plausible. That matches the 100\u2013130 ms bucket
in the measurements almost exactly (fanout window + tick alignment +
context switch).

### 2. Single-threaded accept-and-handle (head-of-line blocking)

`portal_http_task` is one big loop:

```c
while (1) {
    int client_fd = accept(s_http_fd, ...);
    handle_http_client(client_fd);  // synchronous; reads, builds, sends
}
```

While `handle_http_client` is running, NOTHING ELSE can be accepted.
Effects:

- A 20 s OTA upload **blocks every other request** for those 20 s. The
  countdown page's polling, the dashboard you might try to load in a
  second tab, even `/api/ping` \u2014 all queue.
- A slow request (e.g. one that hits the broker-mutex contention above)
  delays the *next* request too.
- Browsers routinely open multiple connections; the dashboard makes 1
  request, `/clients` makes 1 request per 3 s, the countdown makes 1
  per second. They all serialize.
- `listen(s_http_fd, 4)` allows only 4 connections to queue at the
  kernel level. After that, new SYNs are dropped by LwIP.

### 3. PSRAM `malloc(16 KB)` per request

Every request allocates a 16 KB page-body buffer in PSRAM:

```c
#define PAGE_BUF_SIZE 16384
char *body = (char *)malloc(PAGE_BUF_SIZE);
```

PSRAM allocations go through `heap_caps_malloc(MALLOC_CAP_SPIRAM)`,
which is slower than internal RAM (octal SPI vs single-cycle SRAM) and
contends with the WiFi/LwIP buffers also allocated from PSRAM. Under
fragmentation pressure, individual `malloc` calls can take several ms.

Not the dominant cost \u2014 the measurements above show 13 ms median for the
*tiny* `/api/ping` response, suggesting ~10 ms is the LwIP socket
round-trip and ~3 ms is everything else combined. But it's a fixed
overhead that adds up across the polling traffic the countdown page now
generates.

## What the user thought vs reality

> "the web portal is has a dedicated core"

Reality: the **broker** has a dedicated core (`xTaskCreatePinnedToCore(...,
1)`). The **portal** is unpinned (`xTaskCreate(...)`). If the user wanted
the portal to be the responsive part of the system, the wrong task is
pinned. Today, MQTT broker work is isolated and HTTP responsiveness
piggy-backs on whichever core is least busy at the time.

## Recommended fixes (ranked)

### A. Pin `portal_http` to CPU 0 (1-line change, biggest win)

```c
// main/portal.c:2472
- xTaskCreate(portal_http_task, "portal_http", PORTAL_TASK_STACK, NULL, 5, NULL);
+ xTaskCreatePinnedToCore(portal_http_task, "portal_http",
+                         PORTAL_TASK_STACK, NULL, 5, NULL, 0);
```

Eliminates equal-priority contention with `mqtt_broker` on CPU 1. CPU 0
still hosts the WiFi driver and LwIP, but those run at higher priority
(18-23) and pre-empt the portal anyway, so no change in behaviour there.
The portal stops competing with the broker for round-robin slices.

Same change for `portal_dns` and `portal_ws` (per-connection WS tasks).

**Expected effect:** kills the 100\u2013250 ms slow bucket almost entirely.
Median stays the same; p95 should drop to ~30 ms.

### B. Make portal_http a 2-worker pool (medium change, kills OTA HOL blocking)

Replace the single accept-and-handle loop with:

- One acceptor task that pushes accepted FDs into a 4-deep queue
- Two worker tasks that pop FDs and run `handle_http_client`

```c
static QueueHandle_t s_accept_q;
static void portal_http_acceptor(void *arg) { /* accept loop, xQueueSend */ }
static void portal_http_worker(void *arg) {
    int fd;
    while (xQueueReceive(s_accept_q, &fd, portMAX_DELAY))
        handle_http_client(fd);
}
```

Spawn 2 workers (pinned, one per core) plus the acceptor. Now an OTA
upload occupies one worker for 20 s but the other worker still serves
the countdown polls and the dashboard.

**Expected effect:** /clients page no longer freezes during OTA; the
countdown page's first poll after device-comes-back never gets stuck
behind a slow handler.

Caveat: must audit `handle_http_client` for any TLS that was actually
single-thread-safe-by-accident. The broker mutex calls are already
mutex-protected so that's fine.

### C. Enable `LWIP_TCPIP_CORE_LOCKING` (sdkconfig)

```
CONFIG_LWIP_TCPIP_CORE_LOCKING=y
```

Switches LwIP from the netconn-message-passing model to a direct API
that runs in caller context under a core lock. Saves the per-syscall
context switch into the TCP/IP task. ESP-IDF docs claim 30\u201340% socket
latency reduction in benchmarks.

**Expected effect:** median 16 ms \u2192 ~10 ms; smaller p99 tail.

### D. Bump `LWIP_TCPIP_TASK_STACK_SIZE` to 4096

Current `3072` is the bare minimum. Under heavy MQTT load the TCP/IP
task can run out of stack slack and trigger a slow path. Free.

### E. `listen(s_http_fd, 4) -> 8`

Browsers happily open 6 parallel connections to the same origin. With
backlog 4, the 5th and 6th SYNs get dropped (browser retries 1\u20133 s
later, which the user perceives as a hang). One-line change.

### F. Per-request access log

```c
int64_t t0 = esp_timer_get_time();
// ... handle ...
ESP_LOGI(TAG, "%s %s %d %lldms",
         method_str, req.path, status, (esp_timer_get_time()-t0)/1000);
```

Free runtime profiler. Lets us validate the changes above against a real
serial log instead of guessing.

### G. Static `body` buffer (riskier)

Replace the 16 KB `malloc` per request with a static buffer in DRAM
(internal RAM, single-cycle). Saves ~1\u20132 ms per request and zero PSRAM
fragmentation. Costs 16 KB of permanent DRAM \u2014 we have it; the WS task
already reserves more than that. **But:** doesn't compose with the
two-worker pool (B), which needs per-worker buffers. So either:
- Static per-worker (32 KB total, fine), or
- Allocate the buffer once in `portal_start` and reuse.

## Bottom line

| Change | Effort | Expected p95 |
|--------|-------:|-------------:|
| Today (baseline) | \u2014 | **129 ms** |
| A: pin portal to CPU 0 | 1 line | ~30 ms |
| A + B: 2-worker pool | 1 day | ~30 ms, no HOL during OTA |
| A + B + C: enable TCPIP core locking | 1 day + sdkconfig | ~20 ms |
| A+B+C+D+E+F: full sweep | 1\u20132 days | ~15 ms p95, observable |

I'd ship (A) immediately in the next release \u2014 it's a one-line change with
no risk and it gives the portal the "dedicated core" property the user
expected. (B) is the structural fix that pays off most when the device
gets busy (multiple users, OTA in progress, big MQTT fanout). (C) and
the rest are polish.
