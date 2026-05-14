/* CSRF protection for the portal.
 *
 * Pattern: synchronizer token + double-submit cookie convenience.
 *
 *   1. csrf_init() at boot generates a 16-byte random token, formats it
 *      as 32 hex chars. Token is module-static, lives in RAM only,
 *      rotates on every device reboot.
 *
 *   2. http_response_start() unconditionally emits
 *      `Set-Cookie: csrf=<hex>; Path=/; SameSite=Strict`
 *      on every response. NOT HttpOnly -- client JS reads it to put in
 *      the X-CSRF-Token header for fetch() calls. (We have effectively
 *      no XSS surface; all dynamic content goes through textContent.)
 *
 *   3. Every state-changing endpoint calls csrf_verify(&req) and
 *      returns 403 if it fails. csrf_verify accepts the token via
 *      either:
 *        - X-CSRF-Token: <hex> request header  (preferred for fetch() and curl)
 *        - csrf=<hex>  form parameter           (for HTML <form> submits)
 *
 *   4. /api/csrf returns {"token":"<hex>"} (auth-gated) so CLI tooling
 *      can fetch the token in one round trip and reuse it. Browser
 *      forms use the hidden-field pattern and never need this.
 *
 * What this does NOT defend against:
 *   - Same-origin XSS (we ship no third-party JS; all content rendered
 *     via textContent; this is mitigation-by-construction).
 *   - Network adversaries on the LAN intercepting plaintext HTTP
 *     (Basic Auth + CSRF are both visible on the wire; HTTPS would
 *     close this but conflicts with the captive-portal flow).
 *   - Credential theft (Basic Auth covers that, separately).
 *
 * The token is constant per boot, so a single GET / per session gives
 * the browser the cookie it needs. Subsequent forms work without
 * additional round trips.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Length of the hex-string token (without trailing NUL). 32 chars =
 * 16 random bytes. */
#define CSRF_TOKEN_HEX_LEN 32

/* Buffer size for storing the hex token + NUL terminator. */
#define CSRF_TOKEN_BUF_SIZE (CSRF_TOKEN_HEX_LEN + 1)

/* Generate a random 16-byte token at boot and format it as 32 hex
 * chars. Idempotent if called more than once (re-rolls the token, but
 * that breaks any in-flight forms; nothing in the codebase calls this
 * twice on purpose). */
void csrf_init(void);

/* Return the hex-encoded token. Pointer is to a static buffer; valid
 * for the lifetime of the process. Never NULL after csrf_init(). */
const char *csrf_token_hex(void);

/* Validate the X-CSRF-Token header (if non-empty) OR the csrf form
 * parameter (if non-empty) against the active token. Returns true
 * exactly when one of them matches; false otherwise.
 *
 * `header_token` may be NULL or "" -- treated as absent.
 * `body` may be NULL or "" -- form parameter check is skipped.
 *
 * Constant-time compare on the header path to defang timing attacks
 * on the token. Form-path uses standard strcmp (the body parser
 * already early-aborts on non-match in the urldecode loop, so timing
 * leakage is bounded by the body size, not the token value).
 */
bool csrf_verify(const char *header_token, const char *body);
