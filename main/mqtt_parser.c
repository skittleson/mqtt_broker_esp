/*
 * MQTT 3.1.1 Packet Parser/Serializer
 * Pure C implementation, no external dependencies.
 */

#include "mqtt_parser.h"
#include <string.h>

/* ---------- Remaining Length Codec ---------- */

int mqtt_decode_remaining_length(const uint8_t *buf, size_t buf_len,
                                 uint32_t *value)
{
    uint32_t multiplier = 1;
    uint32_t val = 0;
    size_t i = 0;

    do {
        if (i >= buf_len) return 0;   /* need more data */
        if (i >= 4) return -1;        /* malformed */
        val += (uint32_t)(buf[i] & 0x7F) * multiplier;
        multiplier *= 128;
        if (!(buf[i] & 0x80)) {
            *value = val;
            return (int)(i + 1);
        }
        i++;
    } while (1);
}

int mqtt_encode_remaining_length(uint8_t *buf, uint32_t value)
{
    int len = 0;
    do {
        uint8_t byte = value % 128;
        value /= 128;
        if (value > 0) byte |= 0x80;
        buf[len++] = byte;
    } while (value > 0);
    return len;
}

/* ---------- Helpers ---------- */

/* Read a 2-byte big-endian uint16 from buffer */
static inline uint16_t read_u16(const uint8_t *buf)
{
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

/* Write a 2-byte big-endian uint16 to buffer */
static inline void write_u16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

/* Read a length-prefixed UTF-8 string. Returns bytes consumed or -1 on error. */
static int read_utf8_string(const uint8_t *buf, size_t buf_len,
                            const char **str, uint16_t *str_len)
{
    if (buf_len < 2) return -1;
    uint16_t len = read_u16(buf);
    if (buf_len < (size_t)(2 + len)) return -1;
    *str = (const char *)(buf + 2);
    *str_len = len;
    return 2 + len;
}

/* ---------- Packet Length ---------- */

int mqtt_packet_length(const uint8_t *buf, size_t buf_len, size_t *pkt_len)
{
    if (buf_len < 2) return 1;  /* need more data */

    uint32_t remaining;
    int rl_bytes = mqtt_decode_remaining_length(buf + 1, buf_len - 1, &remaining);
    if (rl_bytes == 0) return 1;   /* need more data */
    if (rl_bytes < 0) return -1;   /* malformed */

    *pkt_len = 1 + (size_t)rl_bytes + remaining;
    if (buf_len < *pkt_len) return 1;  /* need more data */
    return 0;
}

uint8_t mqtt_packet_type(uint8_t byte0)
{
    uint8_t type = byte0 & 0xF0;
    /* For most packets, the lower nibble has required flag values.
       Return just the type nibble for classification. */
    return type;
}

/* ---------- Parse CONNECT ---------- */

int mqtt_parse_connect(const uint8_t *pkt, size_t pkt_len, mqtt_connect_t *out)
{
    memset(out, 0, sizeof(*out));

    if (pkt_len < 2) return CONNACK_BAD_PROTOCOL;

    /* Skip fixed header to get to variable header */
    uint32_t remaining;
    int rl_bytes = mqtt_decode_remaining_length(pkt + 1, pkt_len - 1, &remaining);
    if (rl_bytes <= 0) return CONNACK_BAD_PROTOCOL;

    size_t hdr_len = 1 + (size_t)rl_bytes;
    const uint8_t *var = pkt + hdr_len;
    size_t var_len = remaining;

    /* Variable header must be at least 10 bytes */
    if (var_len < 10) return CONNACK_BAD_PROTOCOL;

    /* Protocol name: 0x00 0x04 "MQTT" */
    if (var[0] != 0x00 || var[1] != 0x04) return CONNACK_BAD_PROTOCOL;
    if (memcmp(var + 2, "MQTT", 4) != 0) return CONNACK_BAD_PROTOCOL;

    /* Protocol level must be 4 (MQTT 3.1.1) */
    if (var[6] != 0x04) return CONNACK_BAD_PROTOCOL;

    /* Connect flags */
    uint8_t flags = var[7];
    if (flags & 0x01) return CONNACK_BAD_PROTOCOL;  /* reserved bit must be 0 */

    out->connect_flags = flags;
    out->clean_session = (flags >> 1) & 0x01;
    out->has_will      = (flags >> 2) & 0x01;
    out->has_username  = (flags >> 7) & 0x01;
    out->has_password  = (flags >> 6) & 0x01;

    /* Keep alive */
    out->keep_alive = read_u16(var + 8);

    /* Payload starts after 10-byte variable header */
    const uint8_t *payload = var + 10;
    size_t payload_len = var_len - 10;

    /* Client ID (always present) */
    const char *str;
    uint16_t slen;
    int consumed = read_utf8_string(payload, payload_len, &str, &slen);
    if (consumed < 0) return CONNACK_ID_REJECTED;

    if (slen == 0 && !out->clean_session) {
        return CONNACK_ID_REJECTED;
    }

    uint16_t copy_len = slen < MQTT_MAX_CLIENT_ID_LEN - 1 ? slen : MQTT_MAX_CLIENT_ID_LEN - 1;
    memcpy(out->client_id, str, copy_len);
    out->client_id[copy_len] = '\0';
    out->client_id_len = copy_len;

    payload += consumed;
    payload_len -= consumed;

    /* Skip Will Topic + Will Message if present */
    if (out->has_will) {
        consumed = read_utf8_string(payload, payload_len, &str, &slen);
        if (consumed < 0) return CONNACK_BAD_PROTOCOL;
        payload += consumed;
        payload_len -= consumed;

        /* Will message is also length-prefixed */
        consumed = read_utf8_string(payload, payload_len, &str, &slen);
        if (consumed < 0) return CONNACK_BAD_PROTOCOL;
        payload += consumed;
        payload_len -= consumed;
    }

    /* Extract Username if present */
    if (out->has_username) {
        consumed = read_utf8_string(payload, payload_len, &str, &slen);
        if (consumed < 0) return CONNACK_BAD_CREDS;
        uint16_t copy_len = slen < MQTT_MAX_TOPIC_LEN - 1 ? slen : MQTT_MAX_TOPIC_LEN - 1;
        memcpy(out->username, str, copy_len);
        out->username[copy_len] = '\0';
        out->username_len = copy_len;
        payload += consumed;
        payload_len -= consumed;
    }

    /* Extract Password if present */
    if (out->has_password) {
        consumed = read_utf8_string(payload, payload_len, &str, &slen);
        if (consumed < 0) return CONNACK_BAD_CREDS;
        uint16_t copy_len = slen < MQTT_MAX_TOPIC_LEN - 1 ? slen : MQTT_MAX_TOPIC_LEN - 1;
        memcpy(out->password, str, copy_len);
        out->password[copy_len] = '\0';
        out->password_len = copy_len;
        payload += consumed;
        payload_len -= consumed;
    }

    return CONNACK_ACCEPTED;
}

/* ---------- Parse SUBSCRIBE ---------- */

int mqtt_parse_subscribe(const uint8_t *pkt, size_t pkt_len, mqtt_subscribe_t *out)
{
    memset(out, 0, sizeof(*out));

    uint32_t remaining;
    int rl_bytes = mqtt_decode_remaining_length(pkt + 1, pkt_len - 1, &remaining);
    if (rl_bytes <= 0) return -1;

    size_t hdr_len = 1 + (size_t)rl_bytes;
    const uint8_t *var = pkt + hdr_len;
    size_t var_len = remaining;

    if (var_len < 2) return -1;

    /* Packet identifier */
    out->packet_id = read_u16(var);
    const uint8_t *payload = var + 2;
    size_t payload_len = var_len - 2;

    /* Parse topic filters */
    out->count = 0;
    while (payload_len > 0 && out->count < MQTT_MAX_SUBS_PER_PKT) {
        const char *str;
        uint16_t slen;
        int consumed = read_utf8_string(payload, payload_len, &str, &slen);
        if (consumed < 0) return -1;
        payload += consumed;
        payload_len -= consumed;

        /* QoS byte */
        if (payload_len < 1) return -1;
        uint8_t qos = payload[0] & 0x03;
        payload++;
        payload_len--;

        mqtt_sub_topic_t *t = &out->topics[out->count];
        uint16_t copy_len = slen < MQTT_MAX_TOPIC_LEN - 1 ? slen : MQTT_MAX_TOPIC_LEN - 1;
        memcpy(t->topic, str, copy_len);
        t->topic[copy_len] = '\0';
        t->topic_len = copy_len;
        t->qos = qos;
        out->count++;
    }

    if (out->count == 0) return -1;  /* must have at least one topic */
    return 0;
}

/* ---------- Parse UNSUBSCRIBE ---------- */

int mqtt_parse_unsubscribe(const uint8_t *pkt, size_t pkt_len, mqtt_unsubscribe_t *out)
{
    memset(out, 0, sizeof(*out));

    uint32_t remaining;
    int rl_bytes = mqtt_decode_remaining_length(pkt + 1, pkt_len - 1, &remaining);
    if (rl_bytes <= 0) return -1;

    size_t hdr_len = 1 + (size_t)rl_bytes;
    const uint8_t *var = pkt + hdr_len;
    size_t var_len = remaining;

    if (var_len < 2) return -1;

    out->packet_id = read_u16(var);
    const uint8_t *payload = var + 2;
    size_t payload_len = var_len - 2;

    out->count = 0;
    while (payload_len > 0 && out->count < MQTT_MAX_SUBS_PER_PKT) {
        const char *str;
        uint16_t slen;
        int consumed = read_utf8_string(payload, payload_len, &str, &slen);
        if (consumed < 0) return -1;
        payload += consumed;
        payload_len -= consumed;

        uint16_t copy_len = slen < MQTT_MAX_TOPIC_LEN - 1 ? slen : MQTT_MAX_TOPIC_LEN - 1;
        memcpy(out->topics[out->count], str, copy_len);
        out->topics[out->count][copy_len] = '\0';
        out->topic_lens[out->count] = copy_len;
        out->count++;
    }

    if (out->count == 0) return -1;
    return 0;
}

/* ---------- Parse 2-byte-id ack frames (PUBACK/PUBREC/PUBREL/PUBCOMP) ---------- */

int mqtt_parse_ack(const uint8_t *pkt, size_t pkt_len, uint16_t *packet_id)
{
    if (pkt_len < 4) return -1;

    uint32_t remaining;
    int rl_bytes = mqtt_decode_remaining_length(pkt + 1, pkt_len - 1, &remaining);
    if (rl_bytes <= 0) return -1;
    if (remaining < 2) return -1;

    size_t hdr_len = 1 + (size_t)rl_bytes;
    if (pkt_len < hdr_len + 2) return -1;

    *packet_id = read_u16(pkt + hdr_len);
    return 0;
}

/* ---------- Parse PUBLISH ---------- */

int mqtt_parse_publish(const uint8_t *pkt, size_t pkt_len, mqtt_publish_t *out)
{
    memset(out, 0, sizeof(*out));

    if (pkt_len < 2) return -1;

    uint8_t byte0 = pkt[0];
    out->dup    = (byte0 >> 3) & 0x01;
    out->qos    = (byte0 >> 1) & 0x03;
    out->retain = byte0 & 0x01;

    uint32_t remaining;
    int rl_bytes = mqtt_decode_remaining_length(pkt + 1, pkt_len - 1, &remaining);
    if (rl_bytes <= 0) return -1;

    size_t hdr_len = 1 + (size_t)rl_bytes;
    const uint8_t *var = pkt + hdr_len;
    size_t var_len = remaining;

    if (var_len < 2) return -1;

    /* Topic name */
    uint16_t topic_len = read_u16(var);
    if (var_len < (size_t)(2 + topic_len)) return -1;

    uint16_t copy_len = topic_len < MQTT_MAX_TOPIC_LEN - 1 ? topic_len : MQTT_MAX_TOPIC_LEN - 1;
    memcpy(out->topic, (const char *)(var + 2), copy_len);
    out->topic[copy_len] = '\0';
    out->topic_len = copy_len;

    size_t offset = 2 + topic_len;

    /* Packet identifier (only for QoS 1 and 2) */
    if (out->qos > 0) {
        if (var_len < offset + 2) return -1;
        out->packet_id = read_u16(var + offset);
        offset += 2;
    }

    /* Payload */
    out->payload = var + offset;
    out->payload_len = (var_len > offset) ? (uint32_t)(var_len - offset) : 0;

    return 0;
}

/* ---------- Packet Builders ---------- */

int mqtt_build_connack(uint8_t *buf, uint8_t session_present, uint8_t return_code)
{
    buf[0] = 0x20;     /* CONNACK */
    buf[1] = 0x02;     /* remaining length = 2 */
    buf[2] = session_present & 0x01;
    buf[3] = return_code;
    return 4;
}

int mqtt_build_suback(uint8_t *buf, uint16_t packet_id,
                      const uint8_t *return_codes, uint8_t count)
{
    uint32_t remaining = 2 + count;  /* packet id + return codes */
    buf[0] = 0x90;  /* SUBACK */
    int rl = mqtt_encode_remaining_length(buf + 1, remaining);
    int pos = 1 + rl;
    write_u16(buf + pos, packet_id);
    pos += 2;
    memcpy(buf + pos, return_codes, count);
    pos += count;
    return pos;
}

int mqtt_build_unsuback(uint8_t *buf, uint16_t packet_id)
{
    buf[0] = 0xB0;  /* UNSUBACK */
    buf[1] = 0x02;  /* remaining length = 2 */
    write_u16(buf + 2, packet_id);
    return 4;
}

/* All four QoS ack frames have an identical wire layout:
 *   byte 0: type<<4 (+ required flags for PUBREL)
 *   byte 1: 0x02 remaining-length
 *   bytes 2-3: packet identifier (big-endian)
 */
static int build_ack(uint8_t *buf, uint8_t type_byte, uint16_t packet_id)
{
    buf[0] = type_byte;
    buf[1] = 0x02;
    write_u16(buf + 2, packet_id);
    return 4;
}

int mqtt_build_puback(uint8_t *buf, uint16_t packet_id)
{
    return build_ack(buf, MQTT_PKT_PUBACK, packet_id);
}

int mqtt_build_pubrec(uint8_t *buf, uint16_t packet_id)
{
    return build_ack(buf, MQTT_PKT_PUBREC, packet_id);
}

int mqtt_build_pubrel(uint8_t *buf, uint16_t packet_id)
{
    /* PUBREL fixed-header flags MUST be 0010 per [MQTT-3.6.1-1].
     * MQTT_PKT_PUBREL already includes those flag bits (0x62). */
    return build_ack(buf, MQTT_PKT_PUBREL, packet_id);
}

int mqtt_build_pubcomp(uint8_t *buf, uint16_t packet_id)
{
    return build_ack(buf, MQTT_PKT_PUBCOMP, packet_id);
}

int mqtt_build_pingresp(uint8_t *buf)
{
    buf[0] = 0xD0;
    buf[1] = 0x00;
    return 2;
}

int mqtt_build_publish(uint8_t *buf, size_t buf_size,
                       const char *topic, uint16_t topic_len,
                       const uint8_t *payload, uint32_t payload_len,
                       bool retain)
{
    uint32_t remaining = 2 + topic_len + payload_len;  /* QoS 0: no packet id */

    /* Calculate total size needed */
    uint8_t rl_buf[4];
    int rl_bytes = mqtt_encode_remaining_length(rl_buf, remaining);
    size_t total = 1 + (size_t)rl_bytes + remaining;

    if (total > buf_size) return -1;

    int pos = 0;

    /* Fixed header: PUBLISH, QoS 0, retain flag */
    buf[pos++] = 0x30 | (retain ? 0x01 : 0x00);

    /* Remaining length */
    memcpy(buf + pos, rl_buf, rl_bytes);
    pos += rl_bytes;

    /* Topic */
    write_u16(buf + pos, topic_len);
    pos += 2;
    memcpy(buf + pos, topic, topic_len);
    pos += topic_len;

    /* Payload */
    if (payload_len > 0 && payload != NULL) {
        memcpy(buf + pos, payload, payload_len);
        pos += payload_len;
    }

    return pos;
}

/* ---------- Topic Matching ---------- */

/* Check if a topic level starts with '$' (reserved) */
static inline bool topic_level_starts_with_dollar(const char *filter, uint16_t filter_len,
                                                    const char *topic, uint16_t topic_len,
                                                    uint16_t f_off, uint16_t t_off)
{
    /* If filter starts with $ at this level, wildcard is not allowed */
    if (f_off < filter_len && filter[f_off] == '$') {
        /* MQTT 3.1.1: + and # wildcards are NOT allowed in topic filters
           that use $ at the root level. Also, a topic filter with + or #
           must not match a topic whose corresponding level starts with $. */
        return true;
    }
    if (t_off < topic_len && topic[t_off] == '$') {
        return true;
    }
    return false;
}

bool mqtt_topic_matches(const char *filter, uint16_t filter_len,
                         const char *topic, uint16_t topic_len)
{
    uint16_t fi = 0, ti = 0;

    while (fi < filter_len) {
        /* At a level boundary, check $-topic protection */
        if (fi == 0 || filter[fi - 1] == '/') {
            /* $-topic protection (MQTT 3.1.1 §4.7):
               Wildcards (+, #) are NOT allowed in topic filters that match
               topics beginning with $ (reserved topic namespace). */
            bool topic_has_dollar = (ti < topic_len && topic[ti] == '$');
            bool filter_has_dollar = (fi < filter_len && filter[fi] == '$');
            if (topic_has_dollar != filter_has_dollar) {
                /* One has $ at this level, the other doesn't — no match */
                return false;
            }
            /* Both have $ — proceed normally (literal match for $) */
        }

        if (filter[fi] == '#') {
            /* # must be the last character in the filter (MQTT 3.1.1 §4.7) */
            if (fi + 1 < filter_len && filter[fi + 1] != '/') {
                /* Invalid # position — reject the filter */
                return false;
            }
            return true;
        }

        if (filter[fi] == '+') {
            /* + matches exactly one topic level */
            if (ti >= topic_len) return false;
            /* Check $-protection: + cannot match a $-prefixed level */
            if (topic[ti] == '$') return false;
            while (ti < topic_len && topic[ti] != '/') {
                ti++;
            }
            fi++;
            continue;
        }

        /* Literal character match */
        if (ti >= topic_len) {
            /* Topic exhausted but filter has more.
               Special case: filter ends with /# — matches parent */
            if (fi + 1 < filter_len && filter[fi] == '/' && filter[fi + 1] == '#') {
                return true;
            }
            return false;
        }

        if (filter[fi] != topic[ti]) {
            return false;
        }

        fi++;
        ti++;
    }

    /* Both exhausted = match */
    return (fi >= filter_len && ti >= topic_len);
}
