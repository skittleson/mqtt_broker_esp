/*
 * portal_berry.h — HTTP surface for the Berry scripting runtime.
 *
 * Split off from portal.c to keep that file under ~4000 lines.
 * Two handler categories:
 *   - Streaming POST handlers that take ownership of the socket after
 *     portal.c has done auth (needed because script bodies exceed the
 *     512-byte http_request_t.body cap).
 *   - HTML/JSON/text renderers that fill a caller-supplied buffer so
 *     portal.c handles all socket framing via its existing helpers.
 */
#ifndef PORTAL_BERRY_H
#define PORTAL_BERRY_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* POST kinds that use the streaming handler. */
typedef enum {
    PORTAL_BERRY_POST_SAVE      = 0,  /* POST /berry/save        (slot 0 legacy) */
    PORTAL_BERRY_POST_EVAL      = 1,  /* POST /berry/eval        (run once)      */
    PORTAL_BERRY_POST_SLOT_SAVE = 2,  /* POST /berry/slot/N/save (per-slot save) */
} portal_berry_post_kind_t;

/* Stream the request body off the socket, urldecode form fields, then
 * persist (SAVE/SLOT_SAVE) or evaluate (EVAL) the Berry source.
 * Sends the full HTTP response and closes the socket on exit.
 * Auth MUST already have been verified by portal.c.
 * `header_buf` / `header_len` — first chunk read by portal.c.
 * `slot` is only used for PORTAL_BERRY_POST_SLOT_SAVE (0..BERRY_SLOT_COUNT-1). */
void portal_berry_handle_post_stream(int client_fd,
                                     char *header_buf, int header_len,
                                     portal_berry_post_kind_t kind, int slot);

/* Render the /berry script-manager page into `out`. Returns bytes written. */
size_t portal_berry_render_page(char *out, size_t outsz,
                                const char *csrf_token);

/* JSON for GET /api/berry/status. Returns bytes written. */
size_t portal_berry_render_status_json(char *out, size_t outsz);

/* Plain-text snapshot of the log ring buffer for GET /api/berry/log. */
size_t portal_berry_render_log_text(char *out, size_t outsz);

/* Handle POST /api/berry/restart. Returns true on success. */
bool portal_berry_do_restart(const char *csrf_header, const char *body);

/* Handle POST /berry/enable (global toggle). */
bool portal_berry_do_set_enabled(const char *csrf_header, const char *body,
                                 bool enable);

#ifdef __cplusplus
}
#endif

#endif /* PORTAL_BERRY_H */
