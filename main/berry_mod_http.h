/* berry_mod_http.h — Phase 4 `http` native module.
 *
 * Exposes:
 *   http.get(url [, timeout_ms])                         -> [status, body]
 *   http.post(url, body [, content_type [, timeout_ms]]) -> [status, body]
 *
 * Returns a list: r[0] = HTTP status code (int), r[1] = response body (string).
 * On network/timeout error: r[0] = -1, r[1] = error description.
 * Both functions are synchronous on berry_task.
 */
#ifndef BERRY_MOD_HTTP_H
#define BERRY_MOD_HTTP_H

#include "berry.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void berry_mod_http_register(bvm *vm);

/* berry_runtime.c port hook — writes to the log ring buffer. */
void berry_port_stdout_write(const char *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* BERRY_MOD_HTTP_H */
