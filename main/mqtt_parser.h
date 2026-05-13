/*
 * MQTT 3.1.1 Packet Parser/Serializer
 * Pure C, no external dependencies.
 */

#ifndef MQTT_PARSER_H
#define MQTT_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Packet type constants (upper nibble of fixed header byte 0) ---- */
#define MQTT_PKT_CONNECT      0x10
#define MQTT_PKT_CONNACK      0x20
#define MQTT_PKT_PUBLISH      0x30
#define MQTT_PKT_PUBACK       0x40
#define MQTT_PKT_PUBREC       0x50
#define MQTT_PKT_PUBREL       0x62  /* flags 0010 required by spec [MQTT-3.6.1-1] */
#define MQTT_PKT_PUBCOMP      0x70
#define MQTT_PKT_SUBSCRIBE    0x82
#define MQTT_PKT_SUBACK       0x90
#define MQTT_PKT_UNSUBSCRIBE  0xA2
#define MQTT_PKT_UNSUBACK     0xB0
#define MQTT_PKT_PINGREQ      0xC0
#define MQTT_PKT_PINGRESP     0xD0
#define MQTT_PKT_DISCONNECT   0xE0

/* CONNACK return codes */
#define CONNACK_ACCEPTED        0x00
#define CONNACK_BAD_PROTOCOL    0x01
#define CONNACK_ID_REJECTED     0x02
#define CONNACK_UNAVAILABLE     0x03
#define CONNACK_BAD_CREDS       0x04
#define CONNACK_NOT_AUTH        0x05

/* SUBACK return codes */
#define SUBACK_QOS0    0x00
#define SUBACK_QOS1    0x01
#define SUBACK_QOS2    0x02
#define SUBACK_FAILURE 0x80

/* ---- Limits ---- */
#define MQTT_MAX_TOPIC_LEN      128
#define MQTT_MAX_CLIENT_ID_LEN  64
#define MQTT_MAX_SUBS_PER_PKT   32   /* max topic filters in one SUBSCRIBE */

/* ---- Parsed packet structures ---- */

typedef struct {
    char     client_id[MQTT_MAX_CLIENT_ID_LEN];
    uint16_t client_id_len;
    uint16_t keep_alive;        /* seconds */
    uint8_t  connect_flags;
    bool     clean_session;
    bool     has_will;
    bool     has_username;
    bool     has_password;
    char     username[MQTT_MAX_TOPIC_LEN];  /* extracted from CONNECT if has_username */
    uint16_t username_len;
    char     password[MQTT_MAX_TOPIC_LEN];  /* extracted from CONNECT if has_password */
    uint16_t password_len;
} mqtt_connect_t;

typedef struct {
    char    topic[MQTT_MAX_TOPIC_LEN];
    uint16_t topic_len;
    uint8_t  qos;
} mqtt_sub_topic_t;

typedef struct {
    uint16_t        packet_id;
    uint8_t         count;
    mqtt_sub_topic_t topics[MQTT_MAX_SUBS_PER_PKT];
} mqtt_subscribe_t;

typedef struct {
    uint16_t    packet_id;
    uint8_t     count;
    char        topics[MQTT_MAX_SUBS_PER_PKT][MQTT_MAX_TOPIC_LEN];
    uint16_t    topic_lens[MQTT_MAX_SUBS_PER_PKT];
} mqtt_unsubscribe_t;

typedef struct {
    char        topic[MQTT_MAX_TOPIC_LEN];
    uint16_t    topic_len;
    const uint8_t *payload;     /* points into the receive buffer */
    uint32_t    payload_len;
    uint8_t     qos;
    bool        retain;
    bool        dup;
    uint16_t    packet_id;      /* only valid if qos > 0 */
} mqtt_publish_t;

/* ---- Remaining length codec ---- */

/**
 * Decode MQTT variable-length remaining length field.
 * @param buf     pointer to first byte of remaining length (byte 1 of packet)
 * @param buf_len available bytes in buf
 * @param value   decoded value output
 * @return number of bytes consumed (1-4), or -1 on error, or 0 if need more data
 */
int mqtt_decode_remaining_length(const uint8_t *buf, size_t buf_len,
                                 uint32_t *value);

/**
 * Encode remaining length into buffer.
 * @param buf     output buffer (must have room for 4 bytes)
 * @param value   value to encode
 * @return number of bytes written (1-4)
 */
int mqtt_encode_remaining_length(uint8_t *buf, uint32_t value);

/* ---- Packet parsers ---- */

/**
 * Determine total packet length from a receive buffer.
 * @param buf      receive buffer
 * @param buf_len  bytes available
 * @param pkt_len  output: total packet length including fixed header
 * @return 0 on success, -1 on malformed, 1 if need more data
 */
int mqtt_packet_length(const uint8_t *buf, size_t buf_len, size_t *pkt_len);

/**
 * Get the packet type from byte 0.
 * Returns the upper nibble (e.g., 0x10 for CONNECT).
 * For PUBLISH, returns 0x30 (caller checks flags separately).
 */
uint8_t mqtt_packet_type(uint8_t byte0);

/* Parse a CONNECT packet. Returns 0 on success, CONNACK error code on failure. */
int mqtt_parse_connect(const uint8_t *pkt, size_t pkt_len, mqtt_connect_t *out);

/* Parse a SUBSCRIBE packet. Returns 0 on success, -1 on error. */
int mqtt_parse_subscribe(const uint8_t *pkt, size_t pkt_len, mqtt_subscribe_t *out);

/* Parse an UNSUBSCRIBE packet. Returns 0 on success, -1 on error. */
int mqtt_parse_unsubscribe(const uint8_t *pkt, size_t pkt_len, mqtt_unsubscribe_t *out);

/* Parse a PUBLISH packet. Returns 0 on success, -1 on error. */
int mqtt_parse_publish(const uint8_t *pkt, size_t pkt_len, mqtt_publish_t *out);

/**
 * Parse a 2-byte-packet-id ack frame (PUBACK / PUBREC / PUBREL / PUBCOMP).
 * Returns 0 on success and writes the packet id; -1 on malformed.
 * Caller should check the type byte itself.
 */
int mqtt_parse_ack(const uint8_t *pkt, size_t pkt_len, uint16_t *packet_id);

/* ---- Packet builders ---- */

/* Build CONNACK. Returns bytes written to buf. buf must be >= 4 bytes. */
int mqtt_build_connack(uint8_t *buf, uint8_t session_present, uint8_t return_code);

/* Build SUBACK. Returns bytes written. buf must be >= 5 + count. */
int mqtt_build_suback(uint8_t *buf, uint16_t packet_id,
                      const uint8_t *return_codes, uint8_t count);

/* Build UNSUBACK. Returns bytes written (always 4). */
int mqtt_build_unsuback(uint8_t *buf, uint16_t packet_id);

/**
 * Build a PUBACK frame for an inbound QoS-1 PUBLISH.
 * buf must be >= 4 bytes. Returns 4.
 */
int mqtt_build_puback(uint8_t *buf, uint16_t packet_id);

/**
 * Build a PUBREC frame (first ack in QoS-2 inbound). buf must be >= 4 bytes.
 * Not yet used; provided for phase-5 QoS 2 work. Returns 4.
 */
int mqtt_build_pubrec(uint8_t *buf, uint16_t packet_id);

/**
 * Build a PUBREL frame (second step in QoS-2 outbound). buf must be >= 4 bytes.
 * Uses required flags 0010 per [MQTT-3.6.1-1]. Returns 4.
 */
int mqtt_build_pubrel(uint8_t *buf, uint16_t packet_id);

/**
 * Build a PUBCOMP frame (final ack in QoS-2 inbound). buf must be >= 4 bytes.
 * Returns 4.
 */
int mqtt_build_pubcomp(uint8_t *buf, uint16_t packet_id);

/* Build PINGRESP. Returns bytes written (always 2). */
int mqtt_build_pingresp(uint8_t *buf);

/**
 * Build a PUBLISH packet (QoS 0).
 * @param buf          output buffer
 * @param buf_size     size of output buffer
 * @param topic        topic string
 * @param topic_len    topic length
 * @param payload      payload data
 * @param payload_len  payload length
 * @param retain       retain flag
 * @return bytes written, or -1 if buffer too small
 */
int mqtt_build_publish(uint8_t *buf, size_t buf_size,
                       const char *topic, uint16_t topic_len,
                       const uint8_t *payload, uint32_t payload_len,
                       bool retain);

/* ---- Topic matching ---- */

/**
 * Check if a topic name matches a topic filter (with + and # wildcards).
 * Follows MQTT 3.1.1 section 4.7 rules including $-topic protection.
 */
bool mqtt_topic_matches(const char *filter, uint16_t filter_len,
                        const char *topic, uint16_t topic_len);

#endif /* MQTT_PARSER_H */
