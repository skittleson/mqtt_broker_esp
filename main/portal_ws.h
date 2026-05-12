/*
 * portal_ws.h — WebSocket endpoint for the MQTT topic tester web UI.
 *
 * The portal HTTP task detects "GET /ws" with an "Upgrade: websocket"
 * header, performs auth, then hands the already-accepted socket plus the
 * raw request bytes (so we can pull Sec-WebSocket-Key) to this module.
 *
 * This module:
 *   - Verifies the WebSocket upgrade (RFC 6455 §4.2)
 *   - Replies with the 101 Switching Protocols handshake
 *   - Spawns a dedicated FreeRTOS task that pumps broker-tester events
 *     to the browser and handles inbound publish frames
 *
 * Hard limits live in mqtt_broker.h (BROKER_TESTER_MAX_CONSUMERS).
 */

#ifndef PORTAL_WS_H
#define PORTAL_WS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Handle a WebSocket upgrade.
 *
 * @param fd            the already-accepted client socket (this module takes ownership)
 * @param raw_headers   pointer to the raw HTTP request bytes (headers, possibly more)
 * @param raw_len       length of raw_headers
 *
 * On success, spawns a task that owns `fd` and returns true. The caller MUST
 * NOT close `fd` after a true return.
 *
 * On failure, sends an appropriate HTTP error response, closes `fd`, and
 * returns false.
 */
bool portal_ws_handle_upgrade(int fd, const char *raw_headers, size_t raw_len);

#endif /* PORTAL_WS_H */
