/*
 * MQTT 3.1.1 Broker — Core Logic
 *
 * Single-threaded select()-based TCP server with MQTT packet parsing,
 * subscription management, and message routing.
 *
 * Supports:
 *   - 100 concurrent clients
 *   - 2048 total subscriptions (255+ unique topics)
 *   - QoS 0 and QoS 1 in both directions:
 *       • Inbound QoS 1 PUBLISH → broker sends PUBACK after fanout.
 *       • Outbound QoS 1 PUBLISH (to subscribers that granted QoS 1):
 *         tracked in per-client in-flight tables, retried with DUP=1 on
 *         PUBACK timeout (15s, exponential backoff to 60s, 5 retries max).
 *       • SUBACK grants min(requested, 1).
 *     QoS 2 not yet supported (inbound dropped, outbound never selected).
 *   - Wildcard topic matching (+ and #)
 *   - Keep-alive enforcement
 *   - Retained messages (1-week TTL, memory-capped)
 *   - Optional authentication (username/password via CONNECT, per MQTT 3.1.1)
 */

#include "mqtt_broker.h"
#include "mqtt_parser.h"
#include "ntp.h"  /* ntp_is_synced(), ntp_now_us() for $SYS/broker/time */

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "nvs.h"

static const char *TAG = "mqtt_broker";

/* ---- Client State ---- */

#define BROKER_BUF_MAX  65536  /* absolute max for buffer config */

typedef struct {
    int      fd;                                /* socket fd, -1 if unused */
    char     client_id[MQTT_MAX_CLIENT_ID_LEN]; /* MQTT client identifier */
    char     ip[16];                            /* client IP address string */
    uint8_t  *recv_buf;                         /* accumulation buffer (heap-allocated) */
    size_t   recv_len;                          /* bytes in recv_buf */
    uint16_t keep_alive;                        /* negotiated keep-alive (seconds) */
    int64_t  last_activity;                     /* timestamp of last packet (ms) */
    int64_t  connected_at;                      /* timestamp when CONNECT accepted (ms) */
    uint32_t published;                         /* count of accepted PUBLISH packets from this client */
    uint16_t next_packet_id;                    /* outbound QoS-1 packet id source */
    bool     connected;                         /* CONNECT received and accepted */
    bool     authenticated;                     /* credentials validated */
} broker_client_t;

/* ---- Outbound QoS 1 in-flight slot ----
 * One per pending QoS 1 PUBLISH (broker → subscriber). Slots are owned
 * per-client; the topic + payload buffers are PSRAM heap allocations that
 * must be freed via inflight_free_slot() when the message is PUBACK'd,
 * abandoned, or the client disconnects.
 */
typedef struct {
    bool      active;
    uint8_t   retries;          /* 0 = original send, 1..N = retransmissions */
    bool      retain;           /* retain bit to set on resend */
    uint16_t  packet_id;        /* MQTT packet identifier (non-zero) */
    int64_t   next_retry_ms;    /* absolute deadline for the next retry */
    uint16_t  topic_len;
    uint32_t  payload_len;
    char     *topic;            /* PSRAM heap */
    uint8_t  *payload;          /* PSRAM heap */
} broker_inflight_t;

/* ---- Subscription State ---- */

typedef struct {
    bool     active;
    int      client_idx;                        /* index into clients[] */
    char     topic[MQTT_MAX_TOPIC_LEN];
    uint16_t topic_len;
    uint8_t  qos;
} broker_sub_t;

/* ---- Retained Message State ---- */

typedef struct retained_msg {
    struct retained_msg *next;
    char     *topic;            /* heap-allocated topic string */
    uint16_t  topic_len;
    uint8_t  *payload;          /* heap-allocated payload */
    uint32_t  payload_len;
    int64_t   stored_at_ms;     /* timestamp when stored */
} retained_msg_t;

static retained_msg_t *s_retained_head = NULL;  /* linked list head */
static size_t          s_retained_bytes = 0;    /* total bytes used by retained store */
static size_t          s_retained_count = 0;    /* number of retained messages */

/* ---- Runtime retain configuration (loaded from NVS at startup) ---- */
static bool    s_retain_enabled = true;
static int32_t s_retain_ttl_sec = BROKER_RETAIN_TTL_SEC_DEFAULT;

/* ---- Runtime buffer configuration (loaded from NVS at startup) ---- */
static uint16_t s_buf_size = BROKER_RECV_BUF_SIZE_DEFAULT;

/* ---- Globals (allocated from PSRAM in broker_task) ---- */

static broker_client_t  *s_clients = NULL;
static broker_sub_t     *s_subs = NULL;
static uint8_t          *s_send_buf = NULL;
static int              s_server_fd = -1;
static int              s_num_connected = 0;

/* Outbound QoS-1 in-flight tables. Indexed [client_idx][slot]. Allocated
 * from PSRAM in broker_task() once at startup; per-message topic+payload
 * buffers are allocated/freed on demand. */
static broker_inflight_t (*s_inflight)[BROKER_INFLIGHT_PER_CLIENT_MAX] = NULL;
static size_t            s_inflight_bytes = 0;  /* running total of topic+payload bytes */
static uint32_t          s_inflight_dropped = 0; /* abandoned-after-max-retries counter */
static uint32_t          s_inflight_cap_skips = 0; /* QoS 1 downgrades due to mem cap */

#if BROKER_TESTER_ENABLED

/* ---- Tester state ----
 * Producer (events): broker_task only.
 * Consumers (events): N WS tasks.
 * Producer (publish requests): N WS tasks.
 * Consumer (publish requests): broker_task only.
 *
 * The consumer registry is mutated rarely (on WS connect/disconnect) and
 * read on every PUBLISH. Use a portMUX critical section to guard it; the
 * fanout path holds it for a few microseconds only.
 */
typedef struct {
    void *sb;            /* StreamBufferHandle_t (void* to keep header light) */
    bool  active;
} tester_consumer_t;

typedef struct {
    char     topic[BROKER_TESTER_MAX_TOPIC_LEN + 1];
    uint16_t topic_len;
    uint8_t  payload[BROKER_TESTER_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    bool     retain;
    uint8_t  qos;        /* 0 or 1; tester always sets 0, scheduler may set 1 */
} tester_pub_req_t;

static tester_consumer_t  s_tester_consumers[BROKER_TESTER_MAX_CONSUMERS];
static portMUX_TYPE       s_tester_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t      s_tester_pub_q = NULL;
static bool               s_tester_inited = false;
static uint32_t           s_tester_seq = 0;
static broker_tester_stats_t s_tester_stats = {0};

bool broker_tester_init(void)
{
    if (s_tester_inited) return true;
    s_tester_pub_q = xQueueCreate(8, sizeof(tester_pub_req_t));
    if (!s_tester_pub_q) {
        ESP_LOGE(TAG, "tester: queue create failed; tester disabled");
        return false;
    }
    for (int i = 0; i < BROKER_TESTER_MAX_CONSUMERS; i++) {
        s_tester_consumers[i].sb = NULL;
        s_tester_consumers[i].active = false;
    }
    s_tester_inited = true;
    ESP_LOGI(TAG, "tester: initialized (max %d consumers)", BROKER_TESTER_MAX_CONSUMERS);
    return true;
}

int broker_tester_register(void *stream_buffer_handle)
{
    if (!s_tester_inited || !stream_buffer_handle) return -1;
    int slot = -1;
    portENTER_CRITICAL(&s_tester_mux);
    for (int i = 0; i < BROKER_TESTER_MAX_CONSUMERS; i++) {
        if (!s_tester_consumers[i].active) {
            s_tester_consumers[i].sb = stream_buffer_handle;
            s_tester_consumers[i].active = true;
            slot = i;
            break;
        }
    }
    if (slot >= 0) s_tester_stats.consumers++;
    portEXIT_CRITICAL(&s_tester_mux);
    if (slot >= 0) {
        ESP_LOGI(TAG, "tester: consumer registered (slot %d)", slot);
    }
    return slot;
}

void broker_tester_unregister(int slot)
{
    if (slot < 0 || slot >= BROKER_TESTER_MAX_CONSUMERS) return;
    portENTER_CRITICAL(&s_tester_mux);
    if (s_tester_consumers[slot].active) {
        s_tester_consumers[slot].active = false;
        s_tester_consumers[slot].sb = NULL;
        if (s_tester_stats.consumers > 0) s_tester_stats.consumers--;
    }
    portEXIT_CRITICAL(&s_tester_mux);
    ESP_LOGI(TAG, "tester: consumer unregistered (slot %d)", slot);
}

int broker_tester_consumer_count(void)
{
    int n = 0;
    portENTER_CRITICAL(&s_tester_mux);
    for (int i = 0; i < BROKER_TESTER_MAX_CONSUMERS; i++) {
        if (s_tester_consumers[i].active) n++;
    }
    portEXIT_CRITICAL(&s_tester_mux);
    return n;
}

/* Internal: enqueue a publish request with explicit QoS. Used by both the
 * tester WS path (qos=0) and the timer scheduler (qos=0 or 1). */
static bool broker_enqueue_publish(const char *topic, size_t topic_len,
                                   const uint8_t *payload, size_t payload_len,
                                   uint8_t qos, bool retain)
{
    if (!s_tester_inited || !s_tester_pub_q) return false;
    if (!topic || topic_len == 0 || topic_len > BROKER_TESTER_MAX_TOPIC_LEN) return false;
    if (payload_len > BROKER_TESTER_MAX_PAYLOAD_LEN) return false;
    if (payload_len > 0 && !payload) return false;
    if (qos > 1) return false;
    /* Reject wildcards and embedded NULs in publish topics (MQTT 3.1.1 §3.3.2.1). */
    for (size_t i = 0; i < topic_len; i++) {
        char c = topic[i];
        if (c == '#' || c == '+' || c == '\0') return false;
    }

    tester_pub_req_t req;
    memset(&req, 0, sizeof(req));
    memcpy(req.topic, topic, topic_len);
    req.topic[topic_len] = '\0';
    req.topic_len = (uint16_t)topic_len;
    if (payload_len > 0) memcpy(req.payload, payload, payload_len);
    req.payload_len = (uint16_t)payload_len;
    req.retain = retain;
    req.qos = qos;

    if (xQueueSend(s_tester_pub_q, &req, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_tester_mux);
        s_tester_stats.publish_rejected++;
        portEXIT_CRITICAL(&s_tester_mux);
        return false;
    }
    return true;
}

bool broker_tester_request_publish(const char *topic, size_t topic_len,
                                   const uint8_t *payload, size_t payload_len,
                                   bool retain)
{
    return broker_enqueue_publish(topic, topic_len, payload, payload_len,
                                  /*qos=*/0, retain);
}

/* Public API exposed in mqtt_broker.h. Wraps the same queue used by the
 * tester so we don't grow a second producer path. Leading '$' is rejected
 * here too — the scheduler must never publish into the broker's
 * reserved $SYS/ namespace (MQTT 3.1.1 §4.7.2). */
bool broker_publish_local(const char *topic, size_t topic_len,
                          const uint8_t *payload, size_t payload_len,
                          uint8_t qos, bool retain)
{
    if (!topic || topic_len == 0) return false;
    if (topic[0] == '$') return false;
    return broker_enqueue_publish(topic, topic_len, payload, payload_len,
                                  qos, retain);
}

void broker_tester_get_stats(broker_tester_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_tester_mux);
    *out = s_tester_stats;
    portEXIT_CRITICAL(&s_tester_mux);
}

/* Called from broker_task ONLY, after a successful PUBLISH fanout.
 * Builds one event and tries to enqueue it into every active consumer's
 * stream buffer with zero timeout. Drops are counted, not retried. */
static void tester_fanout(const char *topic, uint16_t topic_len,
                          const uint8_t *payload, uint32_t payload_len,
                          bool retain)
{
    if (!s_tester_inited) return;

    /* Fast exit if no consumers (cheap, taken from outside critical section). */
    bool any = false;
    for (int i = 0; i < BROKER_TESTER_MAX_CONSUMERS; i++) {
        if (s_tester_consumers[i].active) { any = true; break; }
    }
    if (!any) return;

    broker_tester_event_t ev;
    memset(&ev, 0, sizeof(ev));
    uint16_t tl = topic_len;
    if (tl > BROKER_TESTER_MAX_TOPIC_LEN) tl = BROKER_TESTER_MAX_TOPIC_LEN;
    memcpy(ev.topic, topic, tl);
    ev.topic[tl] = '\0';
    ev.topic_len = tl;

    uint32_t pl = payload_len;
    if (pl > BROKER_TESTER_MAX_PAYLOAD_LEN) {
        pl = BROKER_TESTER_MAX_PAYLOAD_LEN;
        ev.truncated = 1;
    }
    if (pl > 0) memcpy(ev.payload, payload, pl);
    ev.payload_len = (uint16_t)pl;
    ev.retain = retain ? 1 : 0;
    ev.seq = ++s_tester_seq;

    /* Snapshot consumer handles inside critical section to avoid races with
     * unregister(); send outside the critical section. */
    void *handles[BROKER_TESTER_MAX_CONSUMERS] = {0};
    int n = 0;
    portENTER_CRITICAL(&s_tester_mux);
    s_tester_stats.events_published++;
    for (int i = 0; i < BROKER_TESTER_MAX_CONSUMERS; i++) {
        if (s_tester_consumers[i].active) handles[n++] = s_tester_consumers[i].sb;
    }
    portEXIT_CRITICAL(&s_tester_mux);

    for (int i = 0; i < n; i++) {
        StreamBufferHandle_t sb = (StreamBufferHandle_t)handles[i];
        if (!sb) continue;
        size_t sent = xStreamBufferSend(sb, &ev, sizeof(ev), 0);
        if (sent != sizeof(ev)) {
            portENTER_CRITICAL(&s_tester_mux);
            s_tester_stats.events_dropped++;
            portEXIT_CRITICAL(&s_tester_mux);
        }
    }
}

/* Forward declaration for use inside broker_task. pub_qos is the QoS of
 * the originating PUBLISH (0 or 1); per-subscriber delivery QoS is capped
 * to min(pub_qos, granted) inside handle_publish_internal. */
static void handle_publish_internal(const char *topic, uint16_t topic_len,
                                    const uint8_t *payload, uint32_t payload_len,
                                    bool retain, uint8_t pub_qos);

/* Called from broker_task to drain queued publish requests from WS tasks. */
static void tester_drain_publish_queue(void)
{
    if (!s_tester_pub_q) return;
    tester_pub_req_t req;
    /* Bounded drain: at most 8 per loop iteration so we never starve real clients. */
    for (int i = 0; i < 8; i++) {
        if (xQueueReceive(s_tester_pub_q, &req, 0) != pdTRUE) break;
        portENTER_CRITICAL(&s_tester_mux);
        s_tester_stats.publish_requests++;
        portEXIT_CRITICAL(&s_tester_mux);
        handle_publish_internal(req.topic, req.topic_len,
                                req.payload, req.payload_len, req.retain,
                                req.qos);
    }
}

#else /* !BROKER_TESTER_ENABLED */

/* Stub when tester is compiled out: scheduler still compiles, but every
 * scheduled publish silently fails. timers.c logs this on first failure. */
bool broker_publish_local(const char *topic, size_t topic_len,
                          const uint8_t *payload, size_t payload_len,
                          uint8_t qos, bool retain)
{
    (void)topic; (void)topic_len; (void)payload; (void)payload_len;
    (void)qos; (void)retain;
    return false;
}

#endif /* BROKER_TESTER_ENABLED */

/* ---- Helpers ---- */

static int64_t get_time_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int find_free_client(void)
{
    for (int i = 0; i < BROKER_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0) return i;
    }
    return -1;
}

/* ---- Outbound QoS-1 in-flight management ---- */

static void inflight_free_slot(int client_idx, int slot)
{
    if (!s_inflight) return;
    broker_inflight_t *e = &s_inflight[client_idx][slot];
    if (!e->active) return;

    size_t bytes = (size_t)e->topic_len + e->payload_len;
    if (s_inflight_bytes >= bytes) {
        s_inflight_bytes -= bytes;
    } else {
        s_inflight_bytes = 0;
    }
    free(e->topic);
    free(e->payload);
    e->topic = NULL;
    e->payload = NULL;
    e->active = false;
}

static void inflight_free_all_for_client(int client_idx)
{
    if (!s_inflight) return;
    for (int s = 0; s < BROKER_INFLIGHT_PER_CLIENT_MAX; s++) {
        if (s_inflight[client_idx][s].active) {
            inflight_free_slot(client_idx, s);
        }
    }
}

static int inflight_find_free_slot(int client_idx)
{
    if (!s_inflight) return -1;
    for (int s = 0; s < BROKER_INFLIGHT_PER_CLIENT_MAX; s++) {
        if (!s_inflight[client_idx][s].active) return s;
    }
    return -1;
}

static int inflight_count_for_client(int client_idx)
{
    if (!s_inflight) return 0;
    int n = 0;
    for (int s = 0; s < BROKER_INFLIGHT_PER_CLIENT_MAX; s++) {
        if (s_inflight[client_idx][s].active) n++;
    }
    return n;
}

/* Generate the next QoS-1 packet id for a client, skipping 0 and any value
 * currently held in this client's in-flight table. Returns 0 if no free id
 * could be found (effectively impossible while the per-client slot cap is
 * far below 65535). */
static uint16_t inflight_next_packet_id(int client_idx)
{
    broker_client_t *c = &s_clients[client_idx];
    for (int attempt = 0; attempt < 65535; attempt++) {
        c->next_packet_id++;
        if (c->next_packet_id == 0) c->next_packet_id = 1;
        bool collision = false;
        for (int s = 0; s < BROKER_INFLIGHT_PER_CLIENT_MAX; s++) {
            if (s_inflight[client_idx][s].active &&
                s_inflight[client_idx][s].packet_id == c->next_packet_id) {
                collision = true;
                break;
            }
        }
        if (!collision) return c->next_packet_id;
    }
    return 0;
}

static void client_disconnect(int idx)
{
    broker_client_t *c = &s_clients[idx];
    if (c->fd < 0) return;

    if (c->connected) {
        ESP_LOGI(TAG, "Client '%s' disconnected (slot %d)", c->client_id, idx);
        s_num_connected--;
    }

    /* Free any outbound QoS-1 in-flight messages we were tracking for this
     * client. Without persistent sessions (clean=0) there is nowhere to put
     * them; phase 4 will queue these for resumed sessions. */
    inflight_free_all_for_client(idx);

    close(c->fd);
    c->fd = -1;
    c->connected = false;
    c->authenticated = false;
    c->recv_len = 0;
    c->client_id[0] = '\0';
    c->published = 0;
    c->next_packet_id = 0;

    /* Remove all subscriptions for this client */
    for (int i = 0; i < BROKER_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].active && s_subs[i].client_idx == idx) {
            s_subs[i].active = false;
        }
    }
}

static int find_free_sub(void)
{
    for (int i = 0; i < BROKER_MAX_SUBSCRIPTIONS; i++) {
        if (!s_subs[i].active) return i;
    }
    return -1;
}

/* Send data to a client, handling partial writes. */
static int client_send(int idx, const uint8_t *data, size_t len)
{
    broker_client_t *c = &s_clients[idx];
    if (c->fd < 0) return -1;

    size_t sent = 0;
    int64_t deadline = get_time_ms() + 3000;  /* 3s send deadline */

    while (sent < len) {
        ssize_t n = send(c->fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (get_time_ms() > deadline) {
                    ESP_LOGW(TAG, "Send timeout for client %d, dropping", idx);
                    client_disconnect(idx);
                    return -1;
                }
                /* Wait for socket write readiness using select() — non-blocking */
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(c->fd, &wfds);
                struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };  /* 50ms */
                int ret = select(c->fd + 1, NULL, &wfds, NULL, &tv);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    ESP_LOGW(TAG, "select() for send failed, client %d", idx);
                    client_disconnect(idx);
                    return -1;
                }
                /* select() returned: retry send (should be ready) */
                continue;
            }
            ESP_LOGW(TAG, "Send error for client %d: %d", idx, errno);
            client_disconnect(idx);
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* ---- Retained Message Store ---- */

static size_t retained_msg_size(const retained_msg_t *r)
{
    /* Account for struct + topic + payload allocations */
    return sizeof(retained_msg_t) + r->topic_len + 1 + r->payload_len;
}

static size_t get_psram_total(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}

static size_t get_retain_mem_limit(void)
{
    size_t total = get_psram_total();
    if (total == 0) {
        /* No PSRAM — use 80% of free heap as limit */
        total = esp_get_free_heap_size() + s_retained_bytes;
    }
    return (total * BROKER_RETAIN_MEM_PCT) / 100;
}

static void *retain_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(size);
    return p;
}

static void retain_free_msg(retained_msg_t *r)
{
    if (!r) return;
    size_t sz = retained_msg_size(r);
    if (s_retained_bytes >= sz) {
        s_retained_bytes -= sz;
    } else {
        s_retained_bytes = 0;  /* guard against underflow */
    }
    if (s_retained_count > 0) {
        s_retained_count--;
    }
    free(r->topic);
    free(r->payload);
    free(r);
}

/* Remove a retained message from the list and free it. */
static void retain_remove(retained_msg_t *r, retained_msg_t *prev)
{
    if (prev) {
        prev->next = r->next;
    } else {
        s_retained_head = r->next;
    }
    retain_free_msg(r);
}

/* Evict oldest retained messages until we're under the memory limit. */
static void retain_evict_for_space(size_t needed)
{
    size_t limit = get_retain_mem_limit();

    while (s_retained_head && (s_retained_bytes + needed) > limit) {
        /* Find and remove the oldest message */
        retained_msg_t *oldest = s_retained_head;
        retained_msg_t *oldest_prev = NULL;
        retained_msg_t *prev = NULL;

        for (retained_msg_t *r = s_retained_head; r; r = r->next) {
            if (r->stored_at_ms < oldest->stored_at_ms) {
                oldest = r;
                oldest_prev = prev;
            }
            prev = r;
        }

        ESP_LOGW(TAG, "Retain evict (memory): '%s' (%lu bytes, age %llds)",
                 oldest->topic, (unsigned long)oldest->payload_len,
                 (long long)(get_time_ms() - oldest->stored_at_ms) / 1000);
        retain_remove(oldest, oldest_prev);
    }
}

/* Evict expired retained messages (older than TTL). */
static void retain_evict_expired(void)
{
    if (s_retain_ttl_sec <= 0) return;  /* TTL=0 means never expire */

    int64_t now = get_time_ms();
    int64_t ttl_ms = (int64_t)s_retain_ttl_sec * 1000;

    retained_msg_t *prev = NULL;
    retained_msg_t *r = s_retained_head;

    while (r) {
        retained_msg_t *next = r->next;
        if ((now - r->stored_at_ms) > ttl_ms) {
            ESP_LOGI(TAG, "Retain expired: '%s'", r->topic);
            retain_remove(r, prev);
            /* prev stays the same since r was removed */
        } else {
            prev = r;
        }
        r = next;
    }
}

/**
 * Store or update a retained message. Per MQTT 3.1.1 spec:
 * - If payload is non-empty, store/replace the retained message for the topic.
 * - If payload is empty (zero-length), delete the retained message for the topic.
 */
static void retain_store(const char *topic, uint16_t topic_len,
                          const uint8_t *payload, uint32_t payload_len)
{
    /* Empty payload = delete only (MQTT spec 3.3.1.3) */
    if (payload_len == 0) {
        /* Find and remove existing retained message for this topic */
        retained_msg_t *prev = NULL;
        for (retained_msg_t *r = s_retained_head; r; r = r->next) {
            if (r->topic_len == topic_len && memcmp(r->topic, topic, topic_len) == 0) {
                ESP_LOGD(TAG, "Retained message deleted for '%.*s'", topic_len, topic);
                retain_remove(r, prev);
                return;
            }
            prev = r;
        }
        return;  /* no existing message to delete */
    }

    /* Reject oversized messages */
    if (payload_len > BROKER_RETAIN_MAX_MSG_SIZE) {
        ESP_LOGW(TAG, "Retained message too large (%lu bytes) for '%.*s', ignoring",
                 (unsigned long)payload_len, topic_len, topic);
        return;
    }

    /* Allocate the new retained message first (before touching existing data) */
    retained_msg_t *msg = (retained_msg_t *)retain_alloc(sizeof(retained_msg_t));
    if (!msg) { ESP_LOGE(TAG, "Failed to allocate retained message struct"); return; }

    msg->topic = (char *)retain_alloc(topic_len + 1);
    if (!msg->topic) { free(msg); return; }

    msg->payload = (uint8_t *)retain_alloc(payload_len);
    if (!msg->payload) { free(msg->topic); free(msg); return; }

    /* Fill in */
    memcpy(msg->topic, topic, topic_len);
    msg->topic[topic_len] = '\0';
    msg->topic_len = topic_len;
    memcpy(msg->payload, payload, payload_len);
    msg->payload_len = payload_len;
    msg->stored_at_ms = get_time_ms();

    /* Calculate space needed */
    size_t needed = sizeof(retained_msg_t) + topic_len + 1 + payload_len;

    /* Find and remove existing retained message for this topic (if any) */
    retained_msg_t *prev = NULL;
    for (retained_msg_t *r = s_retained_head; r; r = r->next) {
        if (r->topic_len == topic_len && memcmp(r->topic, topic, topic_len) == 0) {
            retain_remove(r, prev);
            break;
        }
        prev = r;
    }

    /* Evict old messages if we'd exceed memory limit */
    retain_evict_for_space(needed);

    /* Insert at head */
    msg->next = s_retained_head;
    s_retained_head = msg;
    s_retained_bytes += needed;
    s_retained_count++;

    ESP_LOGD(TAG, "Retained: '%s' (%lu bytes), store=%lu/%lu bytes, count=%u",
             msg->topic, (unsigned long)payload_len,
             (unsigned long)s_retained_bytes, (unsigned long)get_retain_mem_limit(),
             (unsigned)s_retained_count);
}

/**
 * Deliver all retained messages matching a topic filter to a client.
 * Called after a SUBSCRIBE is processed.
 */
static void retain_deliver(int client_idx, const char *filter, uint16_t filter_len)
{
    for (retained_msg_t *r = s_retained_head; r; r = r->next) {
        if (!mqtt_topic_matches(filter, filter_len, r->topic, r->topic_len))
            continue;

        /* Build PUBLISH with retain flag set */
        int out_len = mqtt_build_publish(s_send_buf, s_buf_size,
                                         r->topic, r->topic_len,
                                         r->payload, r->payload_len,
                                         true);  /* retain = true */
        if (out_len < 0) {
            ESP_LOGW(TAG, "Retained msg too large to send: '%s' (%lu bytes)",
                     r->topic, (unsigned long)r->payload_len);
            continue;
        }

        if (s_clients[client_idx].fd >= 0 && s_clients[client_idx].connected) {
            client_send(client_idx, s_send_buf, out_len);
            ESP_LOGD(TAG, "Delivered retained '%s' to client %d", r->topic, client_idx);
        }
    }
}

/* ---- Packet Handlers ---- */

static void handle_connect(int idx, const uint8_t *pkt, size_t pkt_len)
{
    broker_client_t *c = &s_clients[idx];
    mqtt_connect_t conn;

    int rc = mqtt_parse_connect(pkt, pkt_len, &conn);

#ifdef MQTT_BROKER_AUTH_USERNAME
    /* Auth is enabled — check credentials per MQTT 3.1.1 */
    if (rc == CONNACK_ACCEPTED) {
        bool user_ok = false, pass_ok = false;

#ifdef MQTT_BROKER_AUTH_PASSWORD
        if (conn.has_username && conn.has_password) {
            user_ok = (conn.username_len == strlen(MQTT_BROKER_AUTH_USERNAME) &&
                       strncmp(conn.username, MQTT_BROKER_AUTH_USERNAME, conn.username_len) == 0);
            pass_ok = (conn.password_len == strlen(MQTT_BROKER_AUTH_PASSWORD) &&
                       strncmp(conn.password, MQTT_BROKER_AUTH_PASSWORD, conn.password_len) == 0);
        } else if (conn.has_username && !conn.has_password) {
            user_ok = (conn.username_len == strlen(MQTT_BROKER_AUTH_USERNAME) &&
                       strncmp(conn.username, MQTT_BROKER_AUTH_USERNAME, conn.username_len) == 0);
            pass_ok = false;
        } else if (!conn.has_username && conn.has_password) {
            user_ok = false;
            pass_ok = false;
        }
        /* else: no credentials provided — reject */
#else
        /* Auth enabled but no password configured — only accept username match */
        if (conn.has_username) {
            user_ok = (conn.username_len == strlen(MQTT_BROKER_AUTH_USERNAME) &&
                       strncmp(conn.username, MQTT_BROKER_AUTH_USERNAME, conn.username_len) == 0);
        }
#endif
        if (!user_ok || !pass_ok) {
            rc = CONNACK_BAD_CREDS;
            ESP_LOGI(TAG, "CONNECT rejected — bad credentials from client '%.*s'",
                     conn.client_id_len, conn.client_id);
        }
    }
#endif

    if (rc == CONNACK_ACCEPTED) {
        /* Check if a client with the same ID is already connected */
        for (int i = 0; i < BROKER_MAX_CLIENTS; i++) {
            if (i != idx && s_clients[i].connected &&
                strcmp(s_clients[i].client_id, conn.client_id) == 0) {
                /* Disconnect the old client (per MQTT spec) */
                ESP_LOGW(TAG, "Duplicate client ID '%s', disconnecting old", conn.client_id);
                client_disconnect(i);
                break;
            }
        }

        strncpy(c->client_id, conn.client_id, MQTT_MAX_CLIENT_ID_LEN - 1);
        c->client_id[MQTT_MAX_CLIENT_ID_LEN - 1] = '\0';
        c->keep_alive = conn.keep_alive;
        c->connected = true;
        c->authenticated = true;
        c->last_activity = get_time_ms();
        c->connected_at = get_time_ms();
        s_num_connected++;

#ifdef MQTT_BROKER_AUTH_USERNAME
        ESP_LOGI(TAG, "CONNECT '%s' keepalive=%u (slot %d, total=%d) authenticated",
                 c->client_id, c->keep_alive, idx, s_num_connected);
#else
        ESP_LOGI(TAG, "CONNECT '%s' keepalive=%u (slot %d, total=%d)",
                 c->client_id, c->keep_alive, idx, s_num_connected);
#endif
    }

    uint8_t buf[4];
    mqtt_build_connack(buf, 0, (uint8_t)rc);
    client_send(idx, buf, 4);

    if (rc != CONNACK_ACCEPTED) {
        client_disconnect(idx);
    }
}

static void handle_subscribe(int idx, const uint8_t *pkt, size_t pkt_len)
{
    broker_client_t *c = &s_clients[idx];
    if (!c->authenticated) {
        ESP_LOGW(TAG, "Unauthenticated client %d attempted SUBSCRIBE, disconnecting", idx);
        client_disconnect(idx);
        return;
    }

    mqtt_subscribe_t sub_pkt;
    if (mqtt_parse_subscribe(pkt, pkt_len, &sub_pkt) != 0) {
        ESP_LOGW(TAG, "Malformed SUBSCRIBE from client %d", idx);
        client_disconnect(idx);
        return;
    }

    uint8_t return_codes[MQTT_MAX_SUBS_PER_PKT];

    for (int i = 0; i < sub_pkt.count; i++) {
        /* Grant min(requested, 1). QoS 2 not yet supported — downgrade to 1.
         * The granted value is what we'll honour on fanout (see
         * handle_publish_internal). [MQTT-3.8.4-5] */
        uint8_t granted = sub_pkt.topics[i].qos;
        if (granted > 1) granted = 1;

        /* Check if this client already has this subscription */
        bool found = false;
        for (int j = 0; j < BROKER_MAX_SUBSCRIPTIONS; j++) {
            if (s_subs[j].active && s_subs[j].client_idx == idx &&
                s_subs[j].topic_len == sub_pkt.topics[i].topic_len &&
                memcmp(s_subs[j].topic, sub_pkt.topics[i].topic,
                       sub_pkt.topics[i].topic_len) == 0) {
                /* Update granted QoS in place */
                s_subs[j].qos = granted;
                found = true;
                break;
            }
        }

        if (!found) {
            int slot = find_free_sub();
            if (slot < 0) {
                ESP_LOGW(TAG, "Subscription pool full!");
                return_codes[i] = SUBACK_FAILURE;
                continue;
            }
            s_subs[slot].active = true;
            s_subs[slot].client_idx = idx;
            memcpy(s_subs[slot].topic, sub_pkt.topics[i].topic,
                   sub_pkt.topics[i].topic_len);
            s_subs[slot].topic[sub_pkt.topics[i].topic_len] = '\0';
            s_subs[slot].topic_len = sub_pkt.topics[i].topic_len;
            s_subs[slot].qos = granted;
        }

        return_codes[i] = granted;  /* SUBACK_QOS0 = 0, SUBACK_QOS1 = 1 */
        ESP_LOGD(TAG, "SUB client %d: '%s' (req=%u granted=%u)",
                 idx, sub_pkt.topics[i].topic, sub_pkt.topics[i].qos, granted);
    }

    uint8_t buf[128];
    int len = mqtt_build_suback(buf, sub_pkt.packet_id, return_codes, sub_pkt.count);
    client_send(idx, buf, len);

    /* Deliver retained messages matching the subscribed topics (MQTT 3.3.1.3) */
    for (int i = 0; i < sub_pkt.count; i++) {
        if (return_codes[i] != SUBACK_FAILURE) {
            retain_deliver(idx, sub_pkt.topics[i].topic, sub_pkt.topics[i].topic_len);
        }
    }

    s_clients[idx].last_activity = get_time_ms();
}

static void handle_unsubscribe(int idx, const uint8_t *pkt, size_t pkt_len)
{
    mqtt_unsubscribe_t unsub_pkt;
    if (mqtt_parse_unsubscribe(pkt, pkt_len, &unsub_pkt) != 0) {
        ESP_LOGW(TAG, "Malformed UNSUBSCRIBE from client %d", idx);
        client_disconnect(idx);
        return;
    }

    for (int i = 0; i < unsub_pkt.count; i++) {
        for (int j = 0; j < BROKER_MAX_SUBSCRIPTIONS; j++) {
            if (s_subs[j].active && s_subs[j].client_idx == idx &&
                s_subs[j].topic_len == unsub_pkt.topic_lens[i] &&
                memcmp(s_subs[j].topic, unsub_pkt.topics[i],
                       unsub_pkt.topic_lens[i]) == 0) {
                s_subs[j].active = false;
                ESP_LOGD(TAG, "UNSUB client %d: '%s'", idx, unsub_pkt.topics[i]);
                break;
            }
        }
    }

    uint8_t buf[4];
    int len = mqtt_build_unsuback(buf, unsub_pkt.packet_id);
    client_send(idx, buf, len);

    s_clients[idx].last_activity = get_time_ms();
}

/* Allocate a QoS-1 in-flight slot, generate a packet id, copy topic+payload
 * into PSRAM. Returns slot index >= 0 on success, -1 on failure (table full
 * or memory cap exceeded). On failure the caller should fall back to QoS 0
 * delivery for this subscriber. */
static int inflight_enqueue(int client_idx,
                            const char *topic, uint16_t topic_len,
                            const uint8_t *payload, uint32_t payload_len,
                            bool retain, int64_t now_ms)
{
    if (!s_inflight) return -1;

    size_t bytes = (size_t)topic_len + payload_len;
    if (s_inflight_bytes + bytes > BROKER_INFLIGHT_TOTAL_BYTES_MAX) {
        s_inflight_cap_skips++;
        return -1;
    }

    int slot = inflight_find_free_slot(client_idx);
    if (slot < 0) return -1;

    uint16_t pkid = inflight_next_packet_id(client_idx);
    if (pkid == 0) return -1;

    char *t = (char *)heap_caps_malloc(topic_len, MALLOC_CAP_SPIRAM);
    if (!t) t = (char *)malloc(topic_len);
    if (!t) return -1;
    uint8_t *p = NULL;
    if (payload_len > 0) {
        p = (uint8_t *)heap_caps_malloc(payload_len, MALLOC_CAP_SPIRAM);
        if (!p) p = (uint8_t *)malloc(payload_len);
        if (!p) { free(t); return -1; }
        memcpy(p, payload, payload_len);
    }
    memcpy(t, topic, topic_len);

    broker_inflight_t *e = &s_inflight[client_idx][slot];
    e->active        = true;
    e->retries       = 0;
    e->retain        = retain;
    e->packet_id     = pkid;
    e->next_retry_ms = now_ms + BROKER_INFLIGHT_RETRY_INITIAL_MS;
    e->topic         = t;
    e->topic_len     = topic_len;
    e->payload       = p;
    e->payload_len   = payload_len;
    s_inflight_bytes += bytes;
    return slot;
}

/* Send a QoS-1 PUBLISH from an in-flight slot. Used for both first transmit
 * (dup=false) and retransmits (dup=true). On send error the slot is left
 * intact so the next retry tick will try again. */
static void inflight_transmit(int client_idx, int slot, bool dup)
{
    broker_inflight_t *e = &s_inflight[client_idx][slot];
    int out_len = mqtt_build_publish_ex(s_send_buf, s_buf_size,
                                        e->topic, e->topic_len,
                                        e->payload, e->payload_len,
                                        /*qos=*/1, e->retain, dup, e->packet_id);
    if (out_len < 0) {
        ESP_LOGW(TAG, "QoS1 PUBLISH too large to forward (%lu bytes, pkid=%u)",
                 (unsigned long)e->payload_len, e->packet_id);
        inflight_free_slot(client_idx, slot);
        return;
    }
    client_send(client_idx, s_send_buf, out_len);
}

/* Walk every client's in-flight table and resend any messages whose retry
 * deadline has passed. Called from the main broker_task loop, so it must be
 * cheap when nothing is due (early-exit on inactive slots). */
static void inflight_walk(int64_t now)
{
    if (!s_inflight) return;
    for (int ci = 0; ci < BROKER_MAX_CLIENTS; ci++) {
        if (s_clients[ci].fd < 0 || !s_clients[ci].connected) continue;
        for (int s = 0; s < BROKER_INFLIGHT_PER_CLIENT_MAX; s++) {
            broker_inflight_t *e = &s_inflight[ci][s];
            if (!e->active) continue;
            if (now < e->next_retry_ms) continue;

            if (e->retries >= BROKER_INFLIGHT_RETRY_MAX) {
                ESP_LOGW(TAG, "Abandoning QoS1 msg pkid=%u to client '%s' after %u retries",
                         e->packet_id, s_clients[ci].client_id, e->retries);
                s_inflight_dropped++;
                inflight_free_slot(ci, s);
                continue;
            }

            e->retries++;
            /* Exponential backoff: 15s, 30s, 60s, 60s, 60s. */
            int64_t delay = (int64_t)BROKER_INFLIGHT_RETRY_INITIAL_MS << (e->retries - 1);
            if (delay > BROKER_INFLIGHT_RETRY_MAX_MS) delay = BROKER_INFLIGHT_RETRY_MAX_MS;
            e->next_retry_ms = now + delay;

            ESP_LOGD(TAG, "Resend QoS1 pkid=%u to '%s' retry=%u",
                     e->packet_id, s_clients[ci].client_id, e->retries);
            inflight_transmit(ci, s, /*dup=*/true);
        }
    }
}

/* Internal: fanout a publish that has already been parsed/validated.
 * Called by handle_publish (from real TCP clients) and by the tester drain
 * path (from web UI). Runs on broker_task only.
 *
 * Per [MQTT-3.3.1-9]: the QoS at which a message is delivered to a
 * subscriber is min(publisher_qos, granted_qos). The caller passes the
 * publisher_qos (clamped to 0 or 1 here — QoS 2 is dropped before this).
 */
static void handle_publish_internal(const char *topic, uint16_t topic_len,
                                    const uint8_t *payload, uint32_t payload_len,
                                    bool retain, uint8_t pub_qos)
{
    /* Store/update/delete retained message if retain flag is set */
    if (retain && s_retain_enabled) {
        retain_store(topic, topic_len, payload, payload_len);
    }

    /* Build the QoS-0 forward packet once — used for all QoS-0 subscribers. */
    int q0_len = mqtt_build_publish(s_send_buf, s_buf_size,
                                    topic, topic_len,
                                    payload, payload_len,
                                    false);  /* don't retain on forward */
    if (q0_len < 0) {
        ESP_LOGW(TAG, "PUBLISH too large to forward (%lu bytes)",
                 (unsigned long)payload_len);
        return;
    }

    int64_t now = get_time_ms();

    /* Route to all matching subscribers. We do two-pass send: first all
     * QoS-0 deliveries using the prebuilt buffer, then QoS-1 deliveries one
     * at a time (each rebuilds s_send_buf with a per-recipient packet id). */
    for (int i = 0; i < BROKER_MAX_SUBSCRIPTIONS; i++) {
        if (!s_subs[i].active) continue;
        if (!mqtt_topic_matches(s_subs[i].topic, s_subs[i].topic_len,
                                topic, topic_len)) continue;

        int ci = s_subs[i].client_idx;
        if (s_clients[ci].fd < 0 || !s_clients[ci].connected) continue;

        /* Effective delivery QoS = min(publisher_qos, granted). */
        uint8_t eff_qos = pub_qos < s_subs[i].qos ? pub_qos : s_subs[i].qos;

        if (eff_qos == 0) {
            client_send(ci, s_send_buf, q0_len);
            continue;
        }

        /* QoS 1: try to enqueue in the in-flight table. If the table is
         * full or the memory cap is hit, fall back to a one-shot QoS-0
         * send so the message still propagates (degraded but not lost
         * in the common case where the subscriber is still online). */
        int slot = inflight_enqueue(ci, topic, topic_len,
                                    payload, payload_len, false, now);
        if (slot < 0) {
            ESP_LOGW(TAG, "QoS1 inflight full for '%s' — degrading to QoS 0",
                     s_clients[ci].client_id);
            client_send(ci, s_send_buf, q0_len);
            continue;
        }
        inflight_transmit(ci, slot, /*dup=*/false);
    }

    /* s_send_buf may have been overwritten by QoS-1 transmits; rebuild for
     * tester fanout if needed. (Tester only takes the raw payload.) */

#if BROKER_TESTER_ENABLED
    /* After-fanout hook: deliver to tester consumers. Non-blocking, never
     * fails in a way that affects real-client delivery. */
    tester_fanout(topic, topic_len, payload, payload_len, retain);
#endif
}

static void handle_publish(int idx, const uint8_t *pkt, size_t pkt_len)
{
    broker_client_t *c = &s_clients[idx];
    if (!c->authenticated) {
        ESP_LOGW(TAG, "Unauthenticated client %d attempted PUBLISH, disconnecting", idx);
        client_disconnect(idx);
        return;
    }

    mqtt_publish_t pub;
    if (mqtt_parse_publish(pkt, pkt_len, &pub) != 0) {
        ESP_LOGW(TAG, "Malformed PUBLISH from client %d", idx);
        return;
    }

    /* QoS 2 is not yet supported. Drop silently — publisher will retry with
     * DUP=1. This preserves pre-QoS-1 behaviour for any rare QoS 2 client. */
    if (pub.qos == 2) {
        ESP_LOGW(TAG, "QoS 2 PUBLISH from client %d dropped (not yet supported)", idx);
        return;
    }

    /* Count accepted PUBLISH packets per client (for /clients page stats). */
    c->published++;

    ESP_LOGD(TAG, "PUB qos=%d '%s' (%lu bytes) from client %d",
             pub.qos, pub.topic, (unsigned long)pub.payload_len, idx);

    /* Fanout. Per-subscriber delivery QoS = min(pub.qos, granted_qos) —
     * handled inside handle_publish_internal. */
    handle_publish_internal(pub.topic, pub.topic_len,
                            pub.payload, pub.payload_len, pub.retain,
                            pub.qos);

    /* QoS 1: acknowledge after fanout has been attempted. We send PUBACK
     * unconditionally once fanout has run — the MQTT 3.1.1 spec only requires
     * that we have accepted ownership of the message [MQTT-4.3.2-2], not that
     * every subscriber received it. */
    if (pub.qos == 1) {
        uint8_t ack[4];
        mqtt_build_puback(ack, pub.packet_id);
        client_send(idx, ack, 4);
    }

    s_clients[idx].last_activity = get_time_ms();
}

static void handle_pingreq(int idx)
{
    uint8_t buf[2];
    mqtt_build_pingresp(buf);
    client_send(idx, buf, 2);
    s_clients[idx].last_activity = get_time_ms();
    ESP_LOGD(TAG, "PINGREQ from client %d -> PINGRESP", idx);
}

static void handle_disconnect(int idx)
{
    ESP_LOGI(TAG, "DISCONNECT from client '%s'", s_clients[idx].client_id);
    client_disconnect(idx);
}

/* ---- Process received data for a client ---- */

static void process_client_data(int idx)
{
    broker_client_t *c = &s_clients[idx];

    while (c->recv_len > 0) {
        /* Try to determine packet length */
        size_t pkt_len;
        int rc = mqtt_packet_length(c->recv_buf, c->recv_len, &pkt_len);

        if (rc == 1) break;     /* need more data */
        if (rc < 0) {
            ESP_LOGW(TAG, "Malformed packet from client %d, disconnecting", idx);
            client_disconnect(idx);
            return;
        }

        /* We have a complete packet of pkt_len bytes */
        uint8_t pkt_type = mqtt_packet_type(c->recv_buf[0]);

        if (!c->connected && pkt_type != 0x10) {
            /* First packet must be CONNECT */
            ESP_LOGW(TAG, "Non-CONNECT packet before connection from slot %d", idx);
            client_disconnect(idx);
            return;
        }

        switch (pkt_type) {
            case 0x10:  /* CONNECT */
                handle_connect(idx, c->recv_buf, pkt_len);
                break;
            case 0x30:  /* PUBLISH */
                handle_publish(idx, c->recv_buf, pkt_len);
                break;
            case 0x80:  /* SUBSCRIBE */
                handle_subscribe(idx, c->recv_buf, pkt_len);
                break;
            case 0xA0:  /* UNSUBSCRIBE */
                handle_unsubscribe(idx, c->recv_buf, pkt_len);
                break;
            case 0xC0:  /* PINGREQ */
                handle_pingreq(idx);
                break;
            case 0xE0:  /* DISCONNECT */
                handle_disconnect(idx);
                return;  /* client is gone */
            case 0x40: {  /* PUBACK: client acknowledging our QoS-1 PUBLISH */
                uint16_t pkid = 0;
                if (mqtt_parse_ack(c->recv_buf, pkt_len, &pkid) == 0 && s_inflight) {
                    bool freed = false;
                    for (int s = 0; s < BROKER_INFLIGHT_PER_CLIENT_MAX; s++) {
                        if (s_inflight[idx][s].active &&
                            s_inflight[idx][s].packet_id == pkid) {
                            ESP_LOGD(TAG, "PUBACK pkid=%u from '%s' (retries=%u)",
                                     pkid, c->client_id, s_inflight[idx][s].retries);
                            inflight_free_slot(idx, s);
                            freed = true;
                            break;
                        }
                    }
                    if (!freed) {
                        ESP_LOGD(TAG, "PUBACK pkid=%u from '%s' (no matching slot)",
                                 pkid, c->client_id);
                    }
                }
                c->last_activity = get_time_ms();
                break;
            }
            case 0x50:  /* PUBREC */
            case 0x60:  /* PUBREL (top nibble — flags ignored here) */
            case 0x70:  /* PUBCOMP */
                /* QoS 2 not yet supported. Silently consume rather than
                 * disconnect a misbehaving client. */
                ESP_LOGD(TAG, "Ignoring unsolicited QoS-2 ack 0x%02X from client %d",
                         pkt_type, idx);
                break;
            default:
                ESP_LOGW(TAG, "Unsupported packet type 0x%02X from client %d",
                         pkt_type, idx);
                break;
        }

        /* Remove processed packet from buffer */
        if (c->fd < 0) return;  /* client was disconnected during handling */

        if (pkt_len < c->recv_len) {
            memmove(c->recv_buf, c->recv_buf + pkt_len, c->recv_len - pkt_len);
        }
        c->recv_len -= pkt_len;
    }
}

/* ---- Keep-Alive Enforcement ---- */

static void check_keepalive(void)
{
    int64_t now = get_time_ms();

    for (int i = 0; i < BROKER_MAX_CLIENTS; i++) {
        broker_client_t *c = &s_clients[i];
        if (c->fd < 0 || !c->connected) continue;
        if (c->keep_alive == 0) continue;  /* 0 means no keep-alive */

        int64_t deadline_ms = (int64_t)(c->keep_alive + BROKER_KEEPALIVE_GRACE_SEC) * 1000;
        if ((now - c->last_activity) > deadline_ms) {
            ESP_LOGW(TAG, "Keep-alive timeout for client '%s' (slot %d)", c->client_id, i);
            client_disconnect(i);
        }
    }
}

/* ---- Main Broker Task ---- */

static void broker_task(void *arg)
{
    /* Load retain configuration from NVS */
    {
        nvs_handle_t h;
        if (nvs_open("mqtt_cfg", NVS_READONLY, &h) == ESP_OK) {
            uint8_t val8;
            if (nvs_get_u8(h, "retain_en", &val8) == ESP_OK) {
                s_retain_enabled = (val8 != 0);
            }
            int32_t val32;
            if (nvs_get_i32(h, "retain_ttl", &val32) == ESP_OK) {
                s_retain_ttl_sec = val32;
            }
            uint16_t val16;
            if (nvs_get_u16(h, "buf_size", &val16) == ESP_OK && val16 >= 1024 && val16 <= BROKER_BUF_MAX) {
                s_buf_size = val16;
            }
            nvs_close(h);
        }
        ESP_LOGI(TAG, "Retain config: enabled=%d, ttl=%ld sec",
                 (int)s_retain_enabled, (long)s_retain_ttl_sec);
        ESP_LOGI(TAG, "Buffer size: %u bytes", s_buf_size);
    }

    /* Allocate large arrays — try PSRAM first, fall back to regular heap */
    s_clients = (broker_client_t *)heap_caps_calloc(
        BROKER_MAX_CLIENTS, sizeof(broker_client_t), MALLOC_CAP_SPIRAM);
    if (!s_clients) {
        ESP_LOGW(TAG, "PSRAM alloc failed for clients, trying default heap");
        s_clients = (broker_client_t *)calloc(BROKER_MAX_CLIENTS, sizeof(broker_client_t));
    }

    s_subs = (broker_sub_t *)heap_caps_calloc(
        BROKER_MAX_SUBSCRIPTIONS, sizeof(broker_sub_t), MALLOC_CAP_SPIRAM);
    if (!s_subs) {
        ESP_LOGW(TAG, "PSRAM alloc failed for subs, trying default heap");
        s_subs = (broker_sub_t *)calloc(BROKER_MAX_SUBSCRIPTIONS, sizeof(broker_sub_t));
    }

    s_send_buf = (uint8_t *)heap_caps_malloc(
        s_buf_size, MALLOC_CAP_SPIRAM);
    if (!s_send_buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed for send_buf, trying default heap");
        s_send_buf = (uint8_t *)malloc(s_buf_size);
    }

    /* In-flight tables for outbound QoS-1 (per-client, fixed slots,
     * PSRAM-resident). Per-message topic+payload buffers are allocated on
     * demand under BROKER_INFLIGHT_TOTAL_BYTES_MAX. */
    {
        size_t rowbytes = sizeof(broker_inflight_t) * BROKER_INFLIGHT_PER_CLIENT_MAX;
        s_inflight = heap_caps_calloc(BROKER_MAX_CLIENTS, rowbytes, MALLOC_CAP_SPIRAM);
        if (!s_inflight) {
            ESP_LOGW(TAG, "PSRAM alloc failed for inflight tables, trying default heap");
            s_inflight = calloc(BROKER_MAX_CLIENTS, rowbytes);
        }
    }

    if (!s_clients || !s_subs || !s_send_buf || !s_inflight) {
        ESP_LOGE(TAG, "Failed to allocate broker memory!");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Broker state: clients=%u KB, subs=%u KB, inflight=%u KB, retained=%u KB limit",
             (unsigned)(BROKER_MAX_CLIENTS * sizeof(broker_client_t) / 1024),
             (unsigned)(BROKER_MAX_SUBSCRIPTIONS * sizeof(broker_sub_t) / 1024),
             (unsigned)(BROKER_MAX_CLIENTS * BROKER_INFLIGHT_PER_CLIENT_MAX *
                        sizeof(broker_inflight_t) / 1024),
             (unsigned)(get_psram_total() * BROKER_RETAIN_MEM_PCT / 100 / 1024));

    /* Initialize client slots and allocate recv buffers */
    for (int i = 0; i < BROKER_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
        s_clients[i].connected = false;
        s_clients[i].recv_len = 0;
        s_clients[i].recv_buf = (uint8_t *)heap_caps_malloc(s_buf_size, MALLOC_CAP_SPIRAM);
        if (!s_clients[i].recv_buf) {
            s_clients[i].recv_buf = (uint8_t *)malloc(s_buf_size);
        }
        if (!s_clients[i].recv_buf) {
            ESP_LOGE(TAG, "Failed to allocate recv_buf for client %d!", i);
            vTaskDelete(NULL);
            return;
        }
    }

    /* Subscription pool already zeroed by calloc */

    /* Create server socket */
    s_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "Failed to create server socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* Increase TCP send buffer for high-throughput / retained delivery */
    int sndbuf = 16384;
    setsockopt(s_server_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    set_nonblocking(s_server_fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(BROKER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: %d", errno);
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_server_fd, 8) < 0) {
        ESP_LOGE(TAG, "Listen failed: %d", errno);
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "MQTT broker listening on port %d (max %d clients, %d subs)",
             BROKER_PORT, BROKER_MAX_CLIENTS, BROKER_MAX_SUBSCRIPTIONS);

#if BROKER_TESTER_ENABLED
    /* Init tester subsystem. Failure is non-fatal: broker continues without it. */
    if (!broker_tester_init()) {
        ESP_LOGW(TAG, "tester init failed; broker continues without web UI tester");
    }
#endif

    int64_t last_keepalive_check = get_time_ms();
    int64_t last_stats = get_time_ms();
    int64_t last_sys_time   = get_time_ms();  /* Phase 1 NTP plan: $SYS/broker/time */

    /* ---- Main select() loop ---- */
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(s_server_fd, &read_fds);
        int max_fd = s_server_fd;

        for (int i = 0; i < BROKER_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &read_fds);
                if (s_clients[i].fd > max_fd) max_fd = s_clients[i].fd;
            }
        }

        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = BROKER_SELECT_TIMEOUT_MS * 1000,
        };

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (ready < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select() error: %d, rebuilding fd_set", errno);
            /* select() corrupted read_fds — rebuild it */
            continue;
        }

#if BROKER_TESTER_ENABLED
        /* Drain any publish requests from web UI tester before serving sockets.
         * Internally bounded so it cannot starve real clients. */
        tester_drain_publish_queue();
#endif

        /* Accept new connections */
        if (ready > 0 && FD_ISSET(s_server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &addr_len);

            if (client_fd >= 0) {
                int slot = find_free_client();
                if (slot < 0) {
                    ESP_LOGW(TAG, "Max clients reached (%d), rejecting connection",
                             BROKER_MAX_CLIENTS);
                    close(client_fd);
                } else {
                    set_nonblocking(client_fd);

                    /* Disable Nagle for low latency */
                    int flag = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                    s_clients[slot].fd = client_fd;
                    s_clients[slot].connected = false;
                    s_clients[slot].recv_len = 0;
                    s_clients[slot].client_id[0] = '\0';
                    s_clients[slot].last_activity = get_time_ms();
                    s_clients[slot].connected_at = 0;
                    strncpy(s_clients[slot].ip,
                            inet_ntoa(client_addr.sin_addr),
                            sizeof(s_clients[slot].ip) - 1);
                    s_clients[slot].ip[sizeof(s_clients[slot].ip) - 1] = '\0';

                    ESP_LOGI(TAG, "TCP connection from %s:%d (slot %d)",
                             inet_ntoa(client_addr.sin_addr),
                             ntohs(client_addr.sin_port), slot);
                }
            }
        }

        /* Read data from connected clients */
        if (ready > 0) {
            for (int i = 0; i < BROKER_MAX_CLIENTS; i++) {
                if (s_clients[i].fd < 0) continue;
                if (!FD_ISSET(s_clients[i].fd, &read_fds)) continue;

                broker_client_t *c = &s_clients[i];
                size_t space = s_buf_size - c->recv_len;

                if (space == 0) {
                    ESP_LOGW(TAG, "Recv buffer full for client %d, disconnecting", i);
                    client_disconnect(i);
                    continue;
                }

                ssize_t n = recv(c->fd, c->recv_buf + c->recv_len, space, 0);

                if (n <= 0) {
                    if (n == 0) {
                        ESP_LOGI(TAG, "Client %d closed connection", i);
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        ESP_LOGW(TAG, "Recv error from client %d: %d", i, errno);
                    } else {
                        continue;  /* EAGAIN / EINTR — retry */
                    }
                    client_disconnect(i);
                    continue;
                }

                c->recv_len += (size_t)n;
                process_client_data(i);
            }
        }

        /* Periodic tasks */
        int64_t now = get_time_ms();

        /* Keep-alive check every 5 seconds */
        if ((now - last_keepalive_check) > 5000) {
            check_keepalive();
            last_keepalive_check = now;
        }

        /* $SYS/broker/{time,ntp/...} publishers (plan-ntp-server.md).
         *
         * Cadence: every 10s while NTP is synced. handle_publish_internal
         * does the fanout exactly like an external publisher, so
         * subscribers with $SYS/# or $SYS/broker/+ filters all receive it.
         *
         *   $SYS/broker/time          non-retained, ASCII epoch seconds.
         *                              Skipped when unsynced (would be 0).
         *   $SYS/broker/ntp/synced    retained, "1"/"0". New subscribers
         *                              learn the sync state immediately.
         *   $SYS/broker/ntp/stratum   retained, ASCII 2..16.
         *   $SYS/broker/ntp/served    retained, total SNTP responses we've
         *                              sent since boot. Lets dashboards
         *                              alert on broker reboot loops via
         *                              the counter going backward.
         *
         * The retained ones republish every 10s rather than only on edge
         * because we don't track "last published value" -- the retain
         * store dedupes by topic, so the cost is just one fanout pass and
         * one NVS-RAM update per cycle. */
        if ((now - last_sys_time) > 10000) {
            char payload[24];
            int pl;
            if (ntp_is_synced()) {
                /* Use the drift-compensated reading so $SYS/broker/time
                 * stays close to true-UTC even after a long free-running
                 * stretch. 0.7.1 and earlier published raw
                 * gettimeofday() which drifted with the local oscillator
                 * (~20 ppm = ~1.7s/24h on this hardware). */
                int64_t epoch_us = ntp_now_us_corrected();
                if (epoch_us > 0) {
                    pl = snprintf(payload, sizeof(payload), "%lld",
                                  (long long)(epoch_us / 1000000));
                    static const char topic[] = "$SYS/broker/time";
                    handle_publish_internal(topic, (uint16_t)(sizeof(topic) - 1),
                                            (const uint8_t *)payload, (uint32_t)pl,
                                            /*retain=*/false, /*pub_qos=*/0);
                }
            }
            /* ntp/synced (retained) */
            {
                pl = snprintf(payload, sizeof(payload), "%d",
                              ntp_is_synced() ? 1 : 0);
                static const char topic[] = "$SYS/broker/ntp/synced";
                handle_publish_internal(topic, (uint16_t)(sizeof(topic) - 1),
                                        (const uint8_t *)payload, (uint32_t)pl,
                                        /*retain=*/true, /*pub_qos=*/0);
            }
            /* ntp/stratum + ntp/served (retained) -- pull both via
             * ntp_get_state() to keep the snapshot coherent. */
            {
                ntp_state_t st;
                ntp_get_state(&st);
                pl = snprintf(payload, sizeof(payload), "%u",
                              (unsigned)st.stratum);
                static const char tstratum[] = "$SYS/broker/ntp/stratum";
                handle_publish_internal(tstratum, (uint16_t)(sizeof(tstratum) - 1),
                                        (const uint8_t *)payload, (uint32_t)pl,
                                        /*retain=*/true, /*pub_qos=*/0);
                pl = snprintf(payload, sizeof(payload), "%u",
                              (unsigned)st.served);
                static const char tserved[] = "$SYS/broker/ntp/served";
                handle_publish_internal(tserved, (uint16_t)(sizeof(tserved) - 1),
                                        (const uint8_t *)payload, (uint32_t)pl,
                                        /*retain=*/true, /*pub_qos=*/0);
                /* 0.7.2 drift telemetry. drift_ppm is signed and may be
                 * INT32_MIN when unknown (< 2 syncs / < 60s baseline);
                 * publish empty payload in that case so subscribers can
                 * trivially distinguish 'unknown' from '0 ppm'. */
                if (st.drift_ppm == INT32_MIN) {
                    pl = 0;
                    payload[0] = '\0';
                } else {
                    pl = snprintf(payload, sizeof(payload), "%ld",
                                  (long)st.drift_ppm);
                }
                static const char tdrift[] = "$SYS/broker/ntp/drift_ppm";
                handle_publish_internal(tdrift, (uint16_t)(sizeof(tdrift) - 1),
                                        (const uint8_t *)payload, (uint32_t)pl,
                                        /*retain=*/true, /*pub_qos=*/0);
                pl = snprintf(payload, sizeof(payload), "%ld",
                              (long)st.free_running_s);
                static const char tfr[] = "$SYS/broker/ntp/free_running_s";
                handle_publish_internal(tfr, (uint16_t)(sizeof(tfr) - 1),
                                        (const uint8_t *)payload, (uint32_t)pl,
                                        /*retain=*/true, /*pub_qos=*/0);
            }
            last_sys_time = now;
        }

        /* Walk outbound QoS-1 in-flight tables for due retries. Cheap when
         * idle (most slots are inactive). */
        inflight_walk(now);

        /* Log stats every 30 seconds */
        if ((now - last_stats) > 30000) {
            int sub_count = 0;
            for (int i = 0; i < BROKER_MAX_SUBSCRIPTIONS; i++) {
                if (s_subs[i].active) sub_count++;
            }
            int inflight_total = 0;
            for (int i = 0; i < BROKER_MAX_CLIENTS; i++) {
                inflight_total += inflight_count_for_client(i);
            }
            ESP_LOGI(TAG, "Stats: clients=%d, subs=%d, retained=%u (%lu KB), "
                          "inflight=%d (%lu KB), dropped=%lu, cap_skips=%lu, free_heap=%lu",
                     s_num_connected, sub_count,
                     (unsigned)s_retained_count,
                     (unsigned long)(s_retained_bytes / 1024),
                     inflight_total,
                     (unsigned long)(s_inflight_bytes / 1024),
                     (unsigned long)s_inflight_dropped,
                     (unsigned long)s_inflight_cap_skips,
                     (unsigned long)esp_get_free_heap_size());
            last_stats = now;

            /* Evict expired retained messages during stats check */
            retain_evict_expired();
        }
    }
}

/* ---- Public API ---- */

static int64_t s_broker_start_time = 0;

void broker_start(void)
{
    s_broker_start_time = (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    /* Pin to core 1 (core 0 handles WiFi). 16 KB stack for safety. */
    xTaskCreatePinnedToCore(broker_task, "mqtt_broker", 16384, NULL, 5, NULL, 1);
}

void broker_get_stats(broker_stats_t *stats)
{
    if (!stats) return;

    stats->connected_clients = s_num_connected;
    stats->retained_count = (uint32_t)s_retained_count;
    stats->retained_bytes = (uint32_t)s_retained_bytes;
    stats->free_heap = (uint32_t)esp_get_free_heap_size();
    stats->uptime_ms = (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_broker_start_time;
    stats->retain_enabled = s_retain_enabled;
    stats->retain_ttl_sec = s_retain_ttl_sec;
    stats->buf_size = s_buf_size;

    /* Count active subscriptions */
    int sub_count = 0;
    if (s_subs) {
        for (int i = 0; i < BROKER_MAX_SUBSCRIPTIONS; i++) {
            if (s_subs[i].active) sub_count++;
        }
    }
    stats->active_subscriptions = sub_count;
}

int broker_get_clients(broker_client_info_t *out, int max_out)
{
    if (!out || max_out <= 0 || !s_clients) return 0;

    int64_t now = get_time_ms();
    int n = 0;

    for (int i = 0; i < BROKER_MAX_CLIENTS && n < max_out; i++) {
        if (s_clients[i].fd < 0 || !s_clients[i].connected) continue;

        strncpy(out[n].client_id, s_clients[i].client_id, sizeof(out[n].client_id) - 1);
        out[n].client_id[sizeof(out[n].client_id) - 1] = '\0';
        strncpy(out[n].ip, s_clients[i].ip, sizeof(out[n].ip) - 1);
        out[n].ip[sizeof(out[n].ip) - 1] = '\0';
        out[n].keep_alive = s_clients[i].keep_alive;
        out[n].connected_ms = now - s_clients[i].connected_at;
        out[n].last_active_ms = now - s_clients[i].last_activity;

        /* Count subscriptions for this client */
        int subs = 0;
        if (s_subs) {
            for (int j = 0; j < BROKER_MAX_SUBSCRIPTIONS; j++) {
                if (s_subs[j].active && s_subs[j].client_idx == i) subs++;
            }
        }
        out[n].subscriptions = subs;
        out[n].inflight = inflight_count_for_client(i);
        out[n].published = s_clients[i].published;
        n++;
    }

    return n;
}
