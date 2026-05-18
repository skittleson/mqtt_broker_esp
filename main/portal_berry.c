/*
 * portal_berry.c — HTTP handlers for the Berry scripting runtime.
 *
 * Split off from portal.c (which is already ~4000 lines) per AGENTS.md §3.
 * See portal_berry.h for the public surface + design notes.
 */
#include "portal_berry.h"

#include "berry_runtime.h"
#include "csrf.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "portal_berry";

/* --- HTTP framing helpers ---
 * portal.c's http_response_start / http_send_body are static; we keep
 * this module self-contained by re-implementing the minimal subset we
 * need. Worth a few duplicate lines to avoid widening portal.c's API.
 */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_simple(int fd, const char *status, const char *ctype,
                        const char *body, size_t body_len)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, (unsigned)body_len);
    if (n > 0) send_all(fd, hdr, (size_t)n);
    if (body && body_len > 0) send_all(fd, body, body_len);
}

static void send_plain(int fd, const char *status, const char *msg)
{
    send_simple(fd, status, "text/plain; charset=utf-8", msg, strlen(msg));
}

static void send_redirect(int fd, const char *url)
{
    char body[256];
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html><meta charset='utf-8'>"
        "<title>Redirect</title>"
        "<script>location.href='%s'</script>"
        "<p>Redirecting to <a href='%s'>%s</a></p>",
        url, url, url);
    char hdr[256];
    int m = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 303 See Other\r\n"
        "Location: %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", url, n);
    if (m > 0) send_all(fd, hdr, (size_t)m);
    if (n > 0) send_all(fd, body, (size_t)n);
}

/* --- URL-encoded form parsing (works on bodies larger than 512 B) --- */
static size_t urldecode_into(const char *enc, size_t enc_len, char *out, size_t outsz)
{
    if (!out || outsz == 0) return 0;
    size_t i = 0, o = 0;
    while (i < enc_len && o + 1 < outsz) {
        char c = enc[i];
        if (c == '%' && i + 2 < enc_len) {
            char h[3] = { enc[i+1], enc[i+2], '\0' };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 3;
        } else if (c == '+') {
            out[o++] = ' '; i++;
        } else {
            out[o++] = c; i++;
        }
    }
    out[o] = '\0';
    return o;
}

/* Extract a urlencoded form field of any size from `body` (not NUL-terminated;
 * use body_len). Writes urldecoded value into `out`. Returns bytes written. */
static size_t form_get_field(const char *body, size_t body_len,
                             const char *key, char *out, size_t outsz)
{
    if (!body || !key || !out || outsz == 0) return 0;
    out[0] = '\0';
    size_t key_len = strlen(key);
    size_t i = 0;
    while (i < body_len) {
        /* match "key=" at start or after '&' */
        bool at_start = (i == 0) || (body[i-1] == '&');
        if (at_start && i + key_len < body_len &&
            memcmp(body + i, key, key_len) == 0 && body[i + key_len] == '=') {
            size_t v_start = i + key_len + 1;
            size_t v_end = v_start;
            while (v_end < body_len && body[v_end] != '&') v_end++;
            return urldecode_into(body + v_start, v_end - v_start, out, outsz);
        }
        i++;
    }
    return 0;
}

/* --- Header parsing helpers --- */
static int parse_content_length(const char *hdr)
{
    const char *p = strcasestr(hdr, "Content-Length:");
    if (!p) return -1;
    p += strlen("Content-Length:");
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static void parse_csrf_header(const char *hdr, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *p = strcasestr(hdr, "X-CSRF-Token:");
    if (!p) return;
    p += strlen("X-CSRF-Token:");
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i < outsz - 1) {
        out[i] = p[i]; i++;
    }
    out[i] = '\0';
}

static void parse_csrf_query(const char *first_line, char *out, size_t outsz)
{
    /* first_line is "POST /berry/save?csrf=ABC HTTP/1.1\r\n..." */
    out[0] = '\0';
    const char *q = strstr(first_line, "csrf=");
    if (!q) return;
    /* require ? or & before */
    if (q > first_line && q[-1] != '?' && q[-1] != '&') return;
    q += 5;
    size_t i = 0;
    while (q[i] && q[i] != '&' && q[i] != ' ' && q[i] != '\r' && i < outsz - 1) {
        out[i] = q[i]; i++;
    }
    out[i] = '\0';
}

/* --- HTML escape for printable bodies (page + log + eval result) --- */
static size_t html_escape(const char *in, size_t in_len, char *out, size_t outsz)
{
    if (!out || outsz == 0) return 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 6 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '<': memcpy(out+o, "&lt;",   4); o += 4; break;
        case '>': memcpy(out+o, "&gt;",   4); o += 4; break;
        case '&': memcpy(out+o, "&amp;",  5); o += 5; break;
        case '"': memcpy(out+o, "&quot;", 6); o += 6; break;
        default:  out[o++] = (char)c; break;
        }
    }
    out[o] = '\0';
    return o;
}

/* === Streaming POST handler === */

#define POST_BODY_MAX  8192  /* hard cap; berry_save_script enforces its own 3500 */

void portal_berry_handle_post_stream(int client_fd,
                                     char *header_buf, int header_len,
                                     portal_berry_post_kind_t kind)
{
    /* Find body start (after \r\n\r\n) within header_buf. */
    char *bstart = strstr(header_buf, "\r\n\r\n");
    if (!bstart) {
        send_plain(client_fd, "400 Bad Request", "Malformed request");
        close(client_fd);
        return;
    }
    bstart += 4;
    int body_in_hdr = header_len - (int)(bstart - header_buf);

    int content_length = parse_content_length(header_buf);
    if (content_length < 0 || content_length > POST_BODY_MAX) {
        ESP_LOGW(TAG, "POST: bad content-length=%d (max %d)",
                 content_length, POST_BODY_MAX);
        send_plain(client_fd, "413 Payload Too Large",
                   "Script too large (max 8 KB)");
        close(client_fd);
        return;
    }

    char *body = (char *)malloc((size_t)content_length + 1);
    if (!body) {
        send_plain(client_fd, "500 Internal Server Error", "OOM");
        close(client_fd);
        return;
    }

    /* Copy whatever body bytes are already in header_buf. */
    int copied = body_in_hdr < content_length ? body_in_hdr : content_length;
    if (copied > 0) memcpy(body, bstart, (size_t)copied);

    /* Stream the rest from the socket. */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int received = copied;
    while (received < content_length) {
        ssize_t n = recv(client_fd, body + received, content_length - received, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "POST: recv short (%d/%d, errno=%d)",
                     received, content_length, errno);
            free(body);
            close(client_fd);
            return;
        }
        received += (int)n;
    }
    body[content_length] = '\0';

    /* CSRF: header takes precedence; fall back to body field. */
    char csrf_hdr[CSRF_TOKEN_BUF_SIZE] = "";
    parse_csrf_header(header_buf, csrf_hdr, sizeof(csrf_hdr));
    if (csrf_hdr[0] == '\0') {
        /* Try URL query (POST /berry/save?csrf=xxx) for curl users. */
        parse_csrf_query(header_buf, csrf_hdr, sizeof(csrf_hdr));
    }
    if (!csrf_verify(csrf_hdr[0] ? csrf_hdr : NULL, body)) {
        ESP_LOGW(TAG, "POST: csrf rejected");
        send_plain(client_fd, "403 Forbidden", "CSRF token invalid or missing");
        free(body);
        close(client_fd);
        return;
    }

    /* Decode the `script` field. Heap-alloc so we don't have to size
     * for the worst case on stack. */
    char *script = (char *)malloc((size_t)content_length + 1);
    if (!script) {
        send_plain(client_fd, "500 Internal Server Error", "OOM");
        free(body);
        close(client_fd);
        return;
    }
    size_t script_len = form_get_field(body, (size_t)content_length,
                                       "script", script, (size_t)content_length + 1);

    if (kind == PORTAL_BERRY_POST_SAVE) {
        bool ok = berry_save_script(script, script_len);
        free(script);
        free(body);
        if (ok) {
            send_redirect(client_fd, "/berry?saved=1");
        } else {
            send_plain(client_fd, "500 Internal Server Error",
                       "Failed to save script to NVS");
        }
        close(client_fd);
        return;
    }

    /* PORTAL_BERRY_POST_EVAL — run once, render result inline. */
    char result[512] = "";
    bool ok = berry_eval(script, result, sizeof(result), 3000);
    free(script);
    free(body);

    /* Compose an HTML response: the editor page again with a result box
     * inlined at the top. Simpler than a full re-render: a small status
     * panel plus a "back to editor" link. The page itself auto-refreshes
     * its log pane so the user sees stdout from the snippet there too. */
    char esc[600];
    html_escape(result, strlen(result), esc, sizeof(esc));

    char html[1400];
    int n = snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Berry &mdash; eval</title>"
        "<style>"
        "body{margin:0;background:#1f1f1f;color:#eaeaea;font-family:sans-serif;padding:16px}"
        ".bar{margin-bottom:10px}"
        ".bar a{color:#1fa3ec;text-decoration:none}"
        ".bar a:hover{text-decoration:underline}"
        ".rb{background:#0f0f0f;border:1px solid #444;padding:10px;"
        "border-radius:4px;white-space:pre-wrap;word-wrap:break-word;"
        "font-family:monospace;font-size:13px;max-height:60vh;overflow:auto}"
        ".ok{border-left:4px solid #2e7d32}"
        ".err{border-left:4px solid #c62828}"
        "</style></head><body>"
        "<div class='bar'><a href='/berry'>&larr; back to editor</a> &middot; "
        "<a href='/'>home</a></div>"
        "<h2>Eval %s</h2>"
        "<div class='rb %s'>%s</div>"
        "</body></html>",
        ok ? "ok" : "error",
        ok ? "ok" : "err",
        esc[0] ? esc : "(no result)");
    if (n < 0) n = 0;
    send_simple(client_fd, "200 OK", "text/html; charset=utf-8", html, (size_t)n);
    close(client_fd);
}

/* === Page render === */

size_t portal_berry_render_page(char *out, size_t outsz, const char *csrf_token)
{
    if (!out || outsz == 0) return 0;
    int pos = 0;

    berry_status_t st = {0};
    berry_get_status(&st);

    /* Pull the persisted script + HTML-escape it for the textarea. */
    char *script = (char *)malloc(BERRY_SCRIPT_MAX + 1);
    char *script_esc = (char *)malloc(2 * (BERRY_SCRIPT_MAX + 8));
    size_t script_len = 0;
    if (script && script_esc) {
        script_len = berry_get_script(script, BERRY_SCRIPT_MAX + 1);
        html_escape(script, script_len, script_esc, 2 * (BERRY_SCRIPT_MAX + 8));
    }

    pos += snprintf(out + pos, outsz - pos,
        "<style>"
        ".bgrid{display:flex;flex-direction:column;gap:12px}"
        ".bcard{border:1px solid #444;border-radius:4px;padding:10px;background:#262626}"
        ".bcard h3{margin:0 0 8px 0;font-size:1em;color:#1fa3ec}"
        ".bcard textarea{width:100%%;background:#0f0f0f;color:#eaeaea;"
        "border:1px solid #444;border-radius:3px;padding:8px;"
        "font-family:monospace;font-size:13px;line-height:1.4;"
        "box-sizing:border-box;resize:vertical}"
        ".bcard textarea.script{height:280px}"
        ".bcard textarea.eval{height:80px}"
        ".bcard .meta{font-size:0.85em;color:#888;margin-top:4px}"
        ".bcard button{width:auto !important;line-height:1.6 !important;"
        "padding:6px 14px !important;font-size:0.95em !important;"
        "margin-right:6px}"
        ".bpill{display:inline-block;padding:2px 8px;border-radius:10px;"
        "font-size:0.8em;margin-left:6px}"
        ".bpill.on{background:#1b5e20;color:#a5d6a7}"
        ".bpill.off{background:#7a2222;color:#ffb0b0}"
        "#blog{background:#0f0f0f;border:1px solid #444;border-radius:3px;"
        "padding:8px;font-family:monospace;font-size:12px;line-height:1.4;"
        "white-space:pre-wrap;word-wrap:break-word;height:240px;overflow:auto;"
        "color:#cfcfcf}"
        ".srow{display:flex;flex-wrap:wrap;gap:10px;font-size:0.85em;color:#aaa}"
        ".srow span b{color:#eaeaea}"
        "</style>"
        "<fieldset><legend>&nbsp;Berry scripting&nbsp;</legend>"
        "<div class='bgrid'>");

    /* Status card */
    pos += snprintf(out + pos, outsz - pos,
        "<div class='bcard'>"
        "<h3>Status"
        "<span class='bpill %s'>%s</span>"
        "</h3>"
        "<div class='srow'>"
        "<span>Script <b>%u</b> B</span>"
        "<span>Evals <b>%u</b></span>"
        "<span>Errors <b>%u</b></span>"
        "<span>VM uptime <b>%u</b> s</span>"
        "</div>"
        "<div style='margin-top:8px'>"
        "<form method='POST' action='/berry/enable' style='display:inline'>"
        "<input type='hidden' name='csrf' value='%s'>"
        "<input type='hidden' name='enable' value='%d'>"
        "<button type='submit'>%s scripting</button>"
        "</form>"
        "<form method='POST' action='/berry/restart' style='display:inline'>"
        "<input type='hidden' name='csrf' value='%s'>"
        "<button type='submit'>Restart VM</button>"
        "</form>"
        "</div>"
        "</div>",
        st.enabled ? "on" : "off",
        st.enabled ? "enabled" : "disabled",
        (unsigned)st.script_len,
        st.evals_total,
        st.errors_total,
        (unsigned)(st.uptime_ms / 1000),
        csrf_token,
        st.enabled ? 0 : 1,
        st.enabled ? "Disable" : "Enable",
        csrf_token);

    /* Autoexec editor */
    pos += snprintf(out + pos, outsz - pos,
        "<div class='bcard'>"
        "<h3>autoexec.be</h3>"
        "<form method='POST' action='/berry/save'>"
        "<input type='hidden' name='csrf' value='%s'>"
        "<textarea name='script' class='script' "
        "placeholder='# runs on every boot when scripting is enabled\n"
        "# import mqtt\n"
        "# mqtt.subscribe(\"sensor/+\", def (t,p) print(t,p) end)'>%s</textarea>"
        "<div class='meta'>Saved to NVS \xc2\xb7 max %d bytes \xc2\xb7 "
        "saving restarts the VM and re-runs this script</div>"
        "<div style='margin-top:8px'>"
        "<button type='submit'>Save &amp; restart</button>"
        "</div>"
        "</form>"
        "</div>",
        csrf_token,
        script_esc ? script_esc : "",
        BERRY_SCRIPT_MAX);

    /* Run-once REPL */
    pos += snprintf(out + pos, outsz - pos,
        "<div class='bcard' id='beval'>"
        "<h3>Run once</h3>"
        "<form method='POST' action='/berry/eval' id='bevalform'>"
        "<input type='hidden' name='csrf' value='%s' id='bevcsrf'>"
        "<textarea name='script' class='eval' id='bevalscript' "
        "placeholder='1 + 1'></textarea>"
        "<div class='meta'>Evaluates against the live VM. "
        "Globals defined here persist until restart.</div>"
        "<div style='margin-top:8px'>"
        "<button type='submit' id='bevalrun'>Run</button>"
        "</div>"
        "</form>"
        /* Result box: hidden until first run */
        "<div id='bevalresult' style='display:none;margin-top:10px'>"
        "<div id='bevalout' class='rb'></div>"
        "</div>"
        "</div>",
        csrf_token);

    /* Log pane (polled by JS; degrades to a static snapshot without JS) */
    pos += snprintf(out + pos, outsz - pos,
        "<div class='bcard'>"
        "<h3>Log</h3>"
        "<div id='blog'>loading...</div>"
        "<div class='meta'>Auto-refreshes every 2 s. "
        "<a href='/api/berry/log' style='color:#1fa3ec'>raw</a></div>"
        "</div>");

    pos += snprintf(out + pos, outsz - pos,
        "</div></fieldset>"
        "<script>"
        /* Log auto-poll */
        "(function(){"
        "var el=document.getElementById('blog');"
        "function tick(){"
        "fetch('/api/berry/log',{cache:'no-store'})"
        ".then(function(r){return r.text()})"
        ".then(function(t){el.textContent=t||'(empty)';el.scrollTop=el.scrollHeight})"
        ".catch(function(){});"
        "}"
        "tick();setInterval(tick,2000);"
        "})();"
        /* Run-once inline eval */
        "(function(){"
        "var form=document.getElementById('bevalform');"
        "var btn=document.getElementById('bevalrun');"
        "var out=document.getElementById('bevalout');"
        "var wrap=document.getElementById('bevalresult');"
        "var ta=document.getElementById('bevalscript');"
        "if(!form)return;"
        "form.addEventListener('submit',function(e){"
        "e.preventDefault();"
        /* Refresh CSRF before submit in case the page has been open a while.
         * Fire-and-forget: on failure we fall through with the stale token
         * (the eval endpoint will 403 and we show that as an error). */
        "fetch('/api/csrf',{cache:'no-store'})"
        ".then(function(r){return r.json()})"
        ".then(function(d){"
        "document.getElementById('bevcsrf').value=d.token||'';"
        "return d.token;"
        "})"
        ".catch(function(){return '';})"
        ".then(function(tok){"
        "btn.disabled=true;btn.textContent='Running...';"
        "var body=new URLSearchParams();"
        "body.append('script',ta.value);"
        "body.append('csrf',tok);"
        "return fetch('/berry/eval',{"
        "method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded',"
        "'X-CSRF-Token':tok},"
        "body:body.toString()"
        "});"
        "})"
        ".then(function(r){return r.text();})"
        ".then(function(html){"
        /* Parse result text and class out of the returned HTML page */
        "var tmp=document.createElement('div');"
        "tmp.innerHTML=html;"
        "var rb=tmp.querySelector('.rb');"
        "var isErr=rb&&rb.classList.contains('err');"
        "out.textContent=rb?rb.textContent:'(no result)';"
        "out.className='rb '+(isErr?'err':'ok');"
        "wrap.style.display='';"
        "out.scrollTop=out.scrollHeight;"
        "})"
        ".catch(function(e){"
        "out.textContent='fetch error: '+e;"
        "out.className='rb err';"
        "wrap.style.display='';"
        "})"
        ".finally(function(){"
        "btn.disabled=false;btn.textContent='Run';"
        "});"
        "});"
        "})();"
        "</script>");

    free(script);
    free(script_esc);
    return (size_t)pos;
}

/* === API renderers === */

size_t portal_berry_render_status_json(char *out, size_t outsz)
{
    if (!out || outsz == 0) return 0;
    berry_status_t st = {0};
    berry_get_status(&st);
    int n = snprintf(out, outsz,
        "{\"enabled\":%s,\"running\":%s,"
        "\"script_len\":%u,\"evals_total\":%u,\"errors_total\":%u,"
        "\"uptime_ms\":%u,\"heap_used\":%u,"
        "\"firmware\":\"%s\"}",
        st.enabled ? "true" : "false",
        st.running ? "true" : "false",
        (unsigned)st.script_len,
        st.evals_total,
        st.errors_total,
        (unsigned)st.uptime_ms,
        (unsigned)st.heap_used,
        FW_VERSION);
    return n > 0 ? (size_t)n : 0;
}

size_t portal_berry_render_log_text(char *out, size_t outsz)
{
    return berry_log_snapshot(out, outsz);
}

/* === Control handlers === */

bool portal_berry_do_restart(const char *csrf_header, const char *body)
{
    if (!csrf_verify(csrf_header, body)) return false;
    return berry_restart();
}

bool portal_berry_do_set_enabled(const char *csrf_header, const char *body,
                                 bool enable)
{
    if (!csrf_verify(csrf_header, body)) return false;
    berry_set_enabled(enable);
    return true;
}
