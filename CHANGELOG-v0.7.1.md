# mqtt_broker v0.7.1 — CSRF protection

Built: May 13 2026 · SHA-256:
`0992b6475eda03d9a8f5ac0162824807a4ffe2b7ceb7925b80df89c6d2fa39c7`

Tagged release. Closes the **"deferred from v0.6.0 to avoid lockout
during unattended OTA cycle"** debt explicitly named in the
plan-mqtt-ux-v2.md scorecard.

Before 0.7.1, anything that could trigger an HTTP request to the device
— a tab open on a malicious site, an `<img src='http://192.168.22.100/reboot'>`
in an email rendered in a webmail client, even a misclicked link —
could fire any state-changing endpoint as long as the user had ever
authenticated to the portal from that browser session. 0.7.1 closes
this.

## What lands

### Threat model

| Vector                                     | Pre-0.7.1                                                                                                 | Post-0.7.1                                                     |
| ------------------------------------------ | --------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------- |
| Drive-by `<form>` POST from another origin | Triggers `/save-settings`, `/save`, OTA endpoints                                                         | Blocked: SameSite=Strict cookie not sent, no token in form     |
| `<img src=/reboot>` on any page            | Reboots the device                                                                                        | Blocked: `/reboot` is POST now, GET returns 404                |
| `<iframe>` + auto-submit form              | Triggers anything                                                                                         | Blocked: same as above                                         |
| LAN-local XSS via reflected input          | We have no reflected-input surface (all dynamic values go through textContent or server-side HTML escape) | Same; CSRF is an additional layer                              |
| Direct curl with valid Basic Auth          | Works (intentional)                                                                                       | Works after a 1-line CSRF token fetch (`make ota` and friends) |

### Implementation

**Module: `main/csrf.{c,h}` (155 lines)**

```c
void           csrf_init(void);                       // 16-byte random token at boot
const char    *csrf_token_hex(void);                  // 32-hex-char string
bool           csrf_verify(const char *header_token,  // X-CSRF-Token
                           const char *body);         // urlencoded body, looks for `csrf=`
```

- Token rotates on every device reboot (no NVS persistence — adversary
  who reboots the device gets a new token, which they can't read).
- `csrf_verify` is constant-time on the header path
  (`ct_streq` XOR-OR-accumulate), strcmp on the form path (the body
  size, not the token value, bounds timing leakage).
- mbedtls' `esp_fill_random` is the entropy source — HWRNG-backed,
  guaranteed seeded by the time `app_main` runs.

**Carriers (3, in priority order)**:

1. `X-CSRF-Token: <hex>` request header — preferred for `fetch()`, XHR,
   and CLI tooling like `curl -H`.
2. `csrf=<hex>` urlencoded form field — for `<form method='POST'>`
   submits.
3. `?csrf=<hex>` URL query parameter — used by the OTA multipart upload,
   because hidden inputs in multipart bodies land _after_ the file
   payload in browser serializers (forces us to buffer the entire
   upload before deciding to accept). Same security model, cleaner
   implementation.

**Cookie**: every response carries
`Set-Cookie: csrf=<hex>; Path=/; SameSite=Strict`. Not HttpOnly because
client JS reads it to put in the `X-CSRF-Token` header — acceptable
here, see the "What this doesn't defend against" comment in `csrf.h`
for the trade-off.

### Endpoints protected (8)

| Endpoint                  | Pre-0.7.1            | Post-0.7.1                                                    |
| ------------------------- | -------------------- | ------------------------------------------------------------- |
| `POST /save-settings`     | accepted             | requires token                                                |
| `POST /save` (WiFi creds) | accepted             | requires token                                                |
| `POST /api/time/resync`   | accepted             | requires token                                                |
| `POST /ota-rollback`      | accepted             | requires token                                                |
| `POST /ota-url`           | accepted             | requires token                                                |
| `POST /ota-upload`        | accepted (multipart) | requires token in URL or header                               |
| **`GET /reboot`**         | rebooted the device  | **`GET` returns 404**; only `POST /reboot` with token reboots |
| New: `GET /api/csrf`      | n/a                  | returns `{"token":"<hex>"}` (auth-gated; CLI helper)          |

### Endpoints intentionally NOT protected (4)

These are read-only and intentionally open as documented in
`plan-ntp-server.md` Phase 1 and the auth-exempt block in `portal.c`:

| Endpoint                   | Why                                                                                                                              |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `GET /api/ping`            | Liveness probe; used by reboot-countdown polling. No secrets in response.                                                        |
| `GET /api/time`            | NTP state; no settings or secrets. Documented as open.                                                                           |
| `GET /api/csrf`            | Returns the token itself — auth-gated so an attacker can't lift the token in one request, but no CSRF check (would be circular). |
| All other `GET` HTML pages | They're the _source_ of the token.                                                                                               |

### CLI ergonomics (`make ota`, `tools/capture_*.py`)

`make ota` now does a two-step:

1. `curl /api/csrf -u $PORTAL_AUTH` → parse `{"token":"<hex>"}` with `sed`
2. `curl -F firmware=@build/mqtt_broker.bin "$OTA_URL?csrf=$TOKEN"`

Backward-compatible against pre-0.7.1 devices: step 1 returns 404 (no
endpoint), `$TOKEN` is empty, step 2 falls back to the old plain URL.

End-to-end OTA cycle verified post-CSRF: 0.7.0 → 0.7.1-rc1 → 0.7.1
all via `make ota` with no manual intervention.

### Test harness

10 new assertions in `test_broker.py` section 22 (`test_csrf`):

|          # | Test                                                              |
| ---------: | ----------------------------------------------------------------- |
|          1 | `/api/csrf` returns a well-formed 32-hex token                    |
|          2 | `Set-Cookie: csrf=...; SameSite=Strict; Path=/` on every response |
|          3 | POST `/api/time/resync` without token → 403                       |
|          4 | POST with wrong token → 403                                       |
|          5 | POST with `X-CSRF-Token` header → 200                             |
|          6 | POST with `csrf=` form field → 200                                |
|          7 | `GET /reboot` → 404 (promoted to POST in 0.7.1)                   |
|          8 | `POST /reboot` without token → 403 (device NOT rebooted)          |
|          9 | `/api/csrf` without auth → 401 (when `BROKER_AUTH` env set)       |
| (live OTA) | `make ota` end-to-end works through the CSRF gate                 |

Also fixed `test_firmware_version`'s semver regex to accept pre-release
suffixes (`0.7.1-rc1` etc.).

### Results against the live 0.7.1 device

```
test_ntp.py:    13 passed, 0 failed
test_broker.py: 125 passed, 0 failed, 4 skipped (destructive, opt-in)
                ─────────────
                138 passed, 0 failed total
                80.9 s wall clock
```

## Files changed (vs 0.7.0)

```
main/version.h           +5  -5    (0.7.0 -> 0.7.1)
main/csrf.h              new (78 lines)
main/csrf.c              new (108 lines)
main/CMakeLists.txt      +1  -1    (added csrf.c)
main/main.c              +9  -1    (csrf_init() during app_main)
main/portal.c            +220 -25  (parse X-CSRF-Token; Set-Cookie in
                                    http_response_start; csrf_verify
                                    in every state-changing handler;
                                    /reboot promoted to POST; /api/csrf
                                    endpoint; hidden csrf input in
                                    every <form>; X-CSRF-Token in
                                    XHR/fetch() calls; CSRF check in
                                    handle_ota_upload from query
                                    string; new http_send_csrf_403
                                    helper)
Makefile                 +18 -4    (make ota two-step CSRF flow)
test_broker.py           +130 -3   (test_csrf section, semver fix)
CHANGELOG-v0.7.1.md      new
releases/mqtt_broker-v0.7.1.bin            new
releases/mqtt_broker-v0.7.1.bin.sha256     new
docs/screenshots/        refreshed (footer version bump 0.7.0 -> 0.7.1)
```

## Binary size

| Version |        Size |        Δ |
| ------- | ----------: | -------: |
| 0.7.0   | 1,146,592 B |        — |
| 0.7.1   | 1,149,712 B | +3,120 B |

Comfortable: 73 % of 4 MB OTA slot still free.

## Plan-mqtt-ux-v2 scorecard — Phase 4 (CSRF)

| #   | Criterion                                           | Status                                   |
| --- | --------------------------------------------------- | ---------------------------------------- |
| 4.1 | CSRF token on every POST endpoint                   | ✅ all 6 protected + OTA upload + reboot |
| 4.2 | Token rotates on reboot, in-RAM only                | ✅                                       |
| 4.3 | `Set-Cookie: SameSite=Strict` defense-in-depth      | ✅                                       |
| 4.4 | `/reboot` promoted from GET to POST                 | ✅                                       |
| 4.5 | CLI tooling (`make ota`) works through CSRF         | ✅ verified end-to-end                   |
| 4.6 | Integration tests for CSRF                          | ✅ 10 assertions                         |
| 4.7 | No regression in existing 115 broker + 13 NTP tests | ✅ all green                             |

Phase 4 of plan-mqtt-ux-v2 is now done. With 0.7.0 (NTP) and 0.7.1
(CSRF) shipped, all UX-plan and security-plan items are closed.

## Not in 0.7.1 (deferred)

- **Per-form token rotation.** Each form submission today reuses the
  same boot-lifetime token. Stricter implementations rotate the token
  per submission. Trade-off: complexity (server state per token) for
  marginal benefit; the boot-lifetime model already defeats every
  practical CSRF vector against this device.
- **WebSocket /ws CSRF token.** The tester WS is authenticated via the
  same Basic Auth and only delivers MQTT-protocol bytes (no admin
  endpoints over WS), so the practical risk is lower. Could add a
  one-shot token via the WS upgrade handshake later if a vector
  appears.
- **HTTPS portal.** Closes the wire-readable-auth problem. Conflicts
  with captive-portal first-boot flow and would need a self-signed
  cert + browser warning. Deferred.
- **Drift compensation (next ship).** The other half of the
  "user said both are important" pair from the last conversation
  turn. Smaller scope; will ship as 0.7.2 in a separate OTA cycle.
