/* CSRF token implementation. See csrf.h for the threat model and
 * protocol overview. */

#include "csrf.h"

#include <string.h>
#include <stdint.h>

#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "csrf";

static char s_token_hex[CSRF_TOKEN_BUF_SIZE] = { 0 };

static void bytes_to_hex(const uint8_t *in, size_t in_len, char *out)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2 + 0] = hex_chars[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex_chars[in[i] & 0x0F];
    }
    out[in_len * 2] = '\0';
}

void csrf_init(void)
{
    uint8_t raw[16];
    /* esp_fill_random is the HWRNG-backed PRNG that ESP-IDF guarantees
     * is seeded by the time tasks start running. Safe to call from
     * app_main / portal_init. */
    esp_fill_random(raw, sizeof(raw));
    bytes_to_hex(raw, sizeof(raw), s_token_hex);

    /* Don't log the token itself; that defeats the point. Just confirm
     * we ran. */
    ESP_LOGI(TAG, "csrf token initialized (%d hex chars)",
             CSRF_TOKEN_HEX_LEN);
}

const char *csrf_token_hex(void)
{
    return s_token_hex;
}

/* Constant-time compare. Returns true iff the two NUL-terminated
 * strings are byte-identical AND have the same length, without leaking
 * a match-prefix length through timing. */
static bool ct_streq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return false;
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < la; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* Crude form-parameter lookup that matches `csrf=<value>` anywhere in
 * the body, where the body is url-encoded form data
 * (`k1=v1&k2=v2&csrf=abc...`). Returns a pointer into a heap-allocated
 * copy that the caller does not own -- this is a static scratch
 * buffer, sized for the 32 hex chars + slack.
 *
 * Returns NULL if the parameter is absent or malformed. */
static const char *find_csrf_form_param(const char *body)
{
    static char value[CSRF_TOKEN_BUF_SIZE + 4];
    value[0] = '\0';

    if (!body) return NULL;

    /* Look for "csrf=" preceded by start-of-body or '&'. We need to
     * avoid matching "xcsrf=" or similar; trivially solved by
     * insisting on either body==match or body[match-1]=='&'. */
    const char *p = body;
    while (*p) {
        const char *hit = strstr(p, "csrf=");
        if (!hit) return NULL;
        if (hit == body || *(hit - 1) == '&') {
            hit += 5;  /* skip "csrf=" */
            const char *end = strchr(hit, '&');
            size_t len = end ? (size_t)(end - hit) : strlen(hit);
            if (len >= sizeof(value)) return NULL;
            memcpy(value, hit, len);
            value[len] = '\0';
            return value;
        }
        p = hit + 1;
    }
    return NULL;
}

bool csrf_verify(const char *header_token, const char *body)
{
    /* Prefer the header path -- single str compare, no body parsing,
     * constant-time. */
    if (header_token && header_token[0] != '\0') {
        return ct_streq(header_token, s_token_hex);
    }

    /* Fall back to a form parameter. Used by old-school
     * <form method=POST> submits where the token rides as a hidden
     * input field. */
    const char *form_token = find_csrf_form_param(body);
    if (form_token) {
        return ct_streq(form_token, s_token_hex);
    }

    return false;
}
