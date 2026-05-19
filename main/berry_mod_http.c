/* berry_mod_http.c — Native `http` module for the Berry scripting runtime.
 *
 * Phase 4: synchronous HTTP GET and POST via esp_http_client.
 *
 * Runs on berry_task (CPU 1). Both functions are synchronous — they block
 * berry_task until the response is received or the timeout expires.  This
 * is safe because berry_task is single-threaded and no other callbacks fire
 * while it is blocked; the broker never stalls (it only posts to the queue).
 *
 * API (both registered as globals so no `import` needed):
 *
 *   http.get(url [, timeout_ms=5000]) -> [status, body]
 *   http.post(url, body_str [, content_type="text/plain" [, timeout_ms=5000]])
 *                                      -> [status, body]
 *
 * Returns a list: r[0] = HTTP status code (int), r[1] = body string.
 * On network error or timeout: r[0] = -1, r[1] = error description.
 * Response body is capped at HTTP_RESP_MAX bytes.
 *
 * Usage:
 *   var r = http.get("http://192.168.1.1/api")
 *   print(r[0])   # 200
 *   print(r[1])   # {"result":"ok"}
 */

#include "berry_mod_http.h"
#include "berry_runtime.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "berry.h"

static const char *TAG = "berry_http";

/* Maximum response body we'll read.  Kept small to fit comfortably within
 * Berry's heap budget and leave room for the VM's own stack.  Typical REST
 * responses (Tasmota JSON, webhook confirmations) are well under 1 KB. */
#define HTTP_RESP_MAX  4096
/* Default timeout if the script doesn't pass one. */
#define HTTP_TIMEOUT_DEFAULT_MS  5000

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------*/

/* Push a Berry list instance [status_int, body_string] onto the VM stack.
 * We inject status and body as globals, then build the list via be_loadstring
 * so the result is a proper BE_INSTANCE (list class) that supports [] in scripts.
 *
 * Stack contract: on entry the stack is in the native function's frame.
 * After this call, exactly one value (the list) is pushed on top. */
static void push_result(bvm *vm, int status, const char *body)
{
    /* Inject status into a temporary global. be_setglobal reads top[-1]
     * and does NOT pop it, so we pop after each call. */
    be_pushint(vm, status);
    be_setglobal(vm, "_hs");
    be_pop(vm, 1);

    be_pushstring(vm, body ? body : "");
    be_setglobal(vm, "_hb");
    be_pop(vm, 1);

    /* Build the list. Use 'return' so be_pcall gets the list value back.
     * be_loadstring wraps source in a function; without 'return' the list
     * is created but not returned (nil result). */
    int rc = be_loadstring(vm, "return [_hs, _hb]");
    if (rc == 0) {
        be_pcall(vm, 0);
    } else {
        be_pop(vm, 1);
        be_pushnil(vm);
    }

    /* Clean up temporaries. */
    be_pushnil(vm);  be_setglobal(vm, "_hs");  be_pop(vm, 1);
    be_pushnil(vm);  be_setglobal(vm, "_hb");  be_pop(vm, 1);
}
/* Perform an HTTP request.  Returns heap-allocated body (caller frees) and
 * sets *status_out.  Returns NULL on alloc failure. */
static char *do_request(const char *url, const char *post_body,
                        const char *content_type, int timeout_ms,
                        int *status_out)
{
    *status_out = -1;

    char *resp = (char *)malloc(HTTP_RESP_MAX + 1);
    if (!resp) {
        ESP_LOGE(TAG, "do_request: malloc failed");
        return NULL;
    }
    resp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = timeout_ms,
        .buffer_size = 1024,
        .disable_auto_redirect = false,
        .max_redirection_count = 3,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(resp, HTTP_RESP_MAX, "esp_http_client_init failed");
        return resp;
    }

    if (post_body) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_post_field(client, post_body, (int)strlen(post_body));
        esp_http_client_set_header(client,
            "Content-Type",
            content_type ? content_type : "text/plain");
    }

    esp_err_t err = esp_http_client_open(client, post_body ? (int)strlen(post_body) : 0);
    if (err != ESP_OK) {
        snprintf(resp, HTTP_RESP_MAX, "connect error: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return resp;
    }

    esp_http_client_fetch_headers(client);
    *status_out = esp_http_client_get_status_code(client);

    int total = 0;
    while (total < HTTP_RESP_MAX) {
        int n = esp_http_client_read(client, resp + total, HTTP_RESP_MAX - total);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "%s %s -> HTTP %d (%d bytes)",
             post_body ? "POST" : "GET", url, *status_out, total);
    return resp;
}

/* ---------------------------------------------------------------------------
 * http.get(url [, timeout_ms=5000]) -> map
 * -------------------------------------------------------------------------*/
static int m_get(bvm *vm)
{
    int argc = be_top(vm);
    if (argc < 1 || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "http.get expects (string url [, int timeout_ms])");
        be_return_nil(vm);
    }
    const char *url = be_tostring(vm, 1);
    int timeout_ms = (argc >= 2 && be_isint(vm, 2))
                     ? (int)be_toint(vm, 2) : HTTP_TIMEOUT_DEFAULT_MS;

    int status = -1;
    char *body = do_request(url, NULL, NULL, timeout_ms, &status);
    if (!body) {
        push_result(vm, -1, "out of memory");
        be_return(vm);
    }
    push_result(vm, status, body);
    free(body);
    be_return(vm);
}

/* ---------------------------------------------------------------------------
 * http.post(url, body [, content_type="text/plain" [, timeout_ms=5000]])
 *   -> map
 * -------------------------------------------------------------------------*/
static int m_post(bvm *vm)
{
    int argc = be_top(vm);
    if (argc < 2 || !be_isstring(vm, 1) || !be_isstring(vm, 2)) {
        be_raise(vm, "type_error",
                 "http.post expects (string url, string body "
                 "[, string content_type [, int timeout_ms]])");
        be_return_nil(vm);
    }
    const char *url          = be_tostring(vm, 1);
    const char *post_body    = be_tostring(vm, 2);
    const char *content_type = (argc >= 3 && be_isstring(vm, 3))
                                ? be_tostring(vm, 3) : "text/plain";
    int timeout_ms           = (argc >= 4 && be_isint(vm, 4))
                                ? (int)be_toint(vm, 4) : HTTP_TIMEOUT_DEFAULT_MS;

    int status = -1;
    char *body = do_request(url, post_body, content_type, timeout_ms, &status);
    if (!body) {
        push_result(vm, -1, "out of memory");
        be_return(vm);
    }
    push_result(vm, status, body);
    free(body);
    be_return(vm);
}

/* ---------------------------------------------------------------------------
 * Registration — called from berry_runtime.c::vm_construct()
 * -------------------------------------------------------------------------*/
void berry_mod_http_register(bvm *vm)
{
    if (!vm) return;
    int top = be_top(vm);

    be_newmodule(vm);

    be_pushntvfunction(vm, m_get);
    be_setmember(vm, -2, "get");
    be_pop(vm, 1);

    be_pushntvfunction(vm, m_post);
    be_setmember(vm, -2, "post");
    be_pop(vm, 1);

    be_setglobal(vm, "http");
    be_pop(vm, be_top(vm) - top);

    const char *msg = "[P4] http module registered (get/post)\n";
    berry_port_stdout_write(msg, strlen(msg));
    ESP_LOGI(TAG, "http module registered");
}
