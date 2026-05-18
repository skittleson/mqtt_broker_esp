/*
 * portal_berry.h — HTTP surface for the Berry scripting runtime.
 *
 * Why this is split off from portal.c:
 *   - portal.c is already ~4000 lines; adding the Berry editor page
 *     plus its streaming POST handler would push it past the soft
 *     ceiling called out in AGENTS.md §3.
 *
 * Two kinds of helpers:
 *   - Streaming POST handler (portal_berry_handle_post_stream) that
 *     takes ownership of the socket after portal.c has done auth.
 *     Modeled on handle_ota_upload in portal.c — needed because the
 *     persisted script can exceed the 512-byte http_request_t.body cap.
 *   - HTML / JSON / text renderers that fill a caller-supplied buffer
 *     and let portal.c send via its existing http_send_page /
 *     http_response_start helpers. This keeps all socket framing in
 *     one place.
 */
#ifndef PORTAL_BERRY_H
#define PORTAL_BERRY_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PORTAL_BERRY_POST_SAVE = 0,   /* POST /berry/save  -> persist script  */
    PORTAL_BERRY_POST_EVAL = 1,   /* POST /berry/eval  -> run once, reply */
} portal_berry_post_kind_t;

/* Stream the request body off the socket, urldecode the `script` and
 * `csrf` form fields, and either persist (SAVE) or evaluate (EVAL) the
 * Berry source. Sends the full HTTP response (200 / 303 / 4xx / 5xx)
 * and closes the socket on its way out.
 *
 * Auth MUST already have been verified by portal.c.
 *
 * `header_buf` is the first chunk read by portal.c (headers possibly
 * plus partial body). `header_len` is how many bytes are valid in it.
 * On entry, the socket is at offset `header_len` of the request.
 */
void portal_berry_handle_post_stream(int client_fd,
                                     char *header_buf, int header_len,
                                     portal_berry_post_kind_t kind);

/* Render the /berry editor page into `out`. Returns bytes written.
 * `csrf_token` is the hex token rendered into the page's hidden inputs. */
size_t portal_berry_render_page(char *out, size_t outsz,
                                const char *csrf_token);

/* JSON for GET /api/berry/status. Returns bytes written. */
size_t portal_berry_render_status_json(char *out, size_t outsz);

/* Plain-text snapshot of the log ring buffer for GET /api/berry/log. */
size_t portal_berry_render_log_text(char *out, size_t outsz);

/* Handle POST /api/berry/restart. CSRF check is done here (caller may
 * pass either a header value or the request body for form posts).
 * Returns true on success; caller sends an appropriate response. */
bool portal_berry_do_restart(const char *csrf_header, const char *body);

/* Handle POST /berry/enable (toggle). CSRF check internal. enable=true
 * sets berry_en=1 and restarts; false stops the VM. Returns true on success. */
bool portal_berry_do_set_enabled(const char *csrf_header, const char *body,
                                 bool enable);

#ifdef __cplusplus
}
#endif

#endif /* PORTAL_BERRY_H */
