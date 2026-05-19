# TLS / Certificate Auth Plan

Target firmware: `mqtt_broker` (post-0.6.x, after NTP server lands so cert
`notBefore`/`notAfter` checks have real wall-clock time).
Goal: let the broker accept **MQTTS on port 8883** with a server cert the
device generates or imports itself, and optionally **require client
certificates** so devices authenticate by x509 instead of (or in addition
to) username/password — **no cloud CA, no external PKI, same
plug-it-in promise.**

Companion to:

- `plan-ntp-server.md` — TLS validity windows need real time.
- `plan-mqtt-ux-v2.md` — portal page conventions, reboot countdown, toast UX.
- `docs/qos-persistence-plan.md` — NVS namespace + atomic-write conventions.

---

## Why

Today the broker speaks only plaintext MQTT on `1883`. Three concrete
problems:

1. **Wi-Fi alone is not enough.** Guest VLANs, mesh repeaters, and shared
   home networks routinely expose 1883. Username/password traverses the
   wire base64 in CONNECT — anyone with `tcpdump` gets it.
2. **No identity per device.** Username/password is a single shared secret;
   if one ESP leaks it, every device must rotate. Per-device client certs
   give us revocable identity without a cloud.
3. **Compliance gates.** Several deployments (medical fleet, light
   industrial, audited home labs) flat-out require TLS 1.2+ for any MQTT
   listener, even on a private LAN. We lose those users at "no TLS".

Non-goals: full CA management UI, OCSP/CRL fetching, mutual TLS for the
HTTP portal (separate plan), TLS for the SNTP server (NTS is out of
scope), hardware secure-element key storage (future — see "Future Work").

Target: **TLS 1.2 + TLS 1.3, ECDHE only, RSA-2048 or EC P-256 server
key, ≤ 50 concurrent TLS clients** (down from 100 plain — RAM cost is
real), handshake ≤ 600 ms on ESP32-S3 at 240 MHz with mbedTLS HW accel.

---

## Architecture (one paragraph)

A second listener task (`mqtts_listener`) opens `0.0.0.0:8883`, wraps each
accepted fd in an `esp_tls_t` server context (mbedTLS under the hood,
hardware-accelerated SHA/AES on ESP32-S3), completes the handshake in a
short-lived **handshake worker** so the accept loop never blocks, and then
hands the fully negotiated session off to the **existing** broker client
slot — but with a `tls` pointer alongside `fd` so `mqtt_broker.c` reads
and writes go through `esp_tls_conn_read`/`esp_tls_conn_write` instead of
raw `recv`/`send`. The server cert + key live in NVS blob keys
(`tls/srv_crt`, `tls/srv_key`); a small **self-signed bootstrap cert** is
auto-generated on first boot if none is present, so the device is usable
out-of-the-box. Optional **client-cert auth** is gated by an NVS bool
(`tls/req_client`) + a trusted-CA bundle blob (`tls/ca_crt`); when on,
mbedTLS verifies the chain during handshake and the broker extracts the
client cert's CN (or SAN URI) and uses it as the **authenticated username**
fed into the existing CONNACK auth path — meaning ACLs, `$SYS/clients`,
and the portal "kick" button keep working unchanged.

Everything else (portal page, NVS keys, MQTT `$SYS` topics, tests) is
glue around those two pieces.

---

## NVS schema (namespace `mqtt`, extends `qos-persistence-plan.md`)

| Key              | Type | Default    | Notes                                                |
| ---------------- | ---- | ---------- | ---------------------------------------------------- |
| `tls/enable`     | u8   | `0`        | `1` opens listener on 8883. `0` = TLS off (current). |
| `tls/port`       | u16  | `8883`     | Configurable for non-standard deployments.           |
| `tls/srv_crt`    | blob | _(absent)_ | PEM, ≤ 4 KB. Auto-generated on first enable.         |
| `tls/srv_key`    | blob | _(absent)_ | PEM, ≤ 2 KB. **Never** exposed over portal/API.      |
| `tls/req_client` | u8   | `0`        | `1` ⇒ require client cert (mTLS).                    |
| `tls/ca_crt`     | blob | _(absent)_ | PEM bundle, ≤ 8 KB. Required when `req_client=1`.    |
| `tls/cn_user`    | u8   | `1`        | When mTLS on, use client cert CN as `username`.      |
| `tls/min_ver`    | u8   | `12`       | `12` = TLS1.2, `13` = TLS1.3-only.                   |

Writes go through the same atomic "stage to `tls/srv_crt.new` then
`nvs_commit` then rename" pattern from `qos-persistence-plan.md`, so a
power-loss mid-upload can't brick the listener.

---

## Server cert lifecycle

**On boot, if `tls/enable=1`:**

1. Load `tls/srv_crt` + `tls/srv_key` from NVS.
2. If missing → call `tls_bootstrap_generate_self_signed()`:
   - EC P-256 keypair via mbedTLS `mbedtls_pk_setup` + `ecp_gen_key`
     (uses ESP32-S3 RNG, ~150 ms).
   - CN = `mqtt_broker-<MAC suffix>` (matches mDNS hostname).
   - SAN = `mqtt_broker.local`, `mqtt_broker-<MAC>.local`, and the
     current STA + ETH IPs at gen time.
   - `notBefore` = now (requires NTP — see "Boot ordering" below).
   - `notAfter` = now + 10 years (self-signed; users can replace).
   - Persist both blobs, log fingerprint to console.
3. Pass to `esp_tls_cfg_server_t.servercert_buf` / `serverkey_buf`.

**Replacing the cert** — portal POSTs PEM files; broker validates with
`mbedtls_x509_crt_parse` + `mbedtls_pk_parse_key` before committing.
On commit, a `tls_reload` event closes the listener socket; the listener
task re-creates `esp_tls_cfg_server_t` from the new NVS blobs. Existing
sessions stay up (their `esp_tls_t` still holds the old cert) until the
client disconnects — matches mosquitto's `SIGHUP` behavior.

---

## Boot ordering (subtle)

TLS handshakes fail outright if `notBefore` is in the future, and
self-signed cert generation needs a real timestamp. So:

```
WiFi/ETH GOT_IP  →  SNTP sync (plan-ntp-server.md)
                 →  if cert missing: generate (uses real time)
                 →  start mqtts_listener
```

If NTP sync hasn't completed within `MQTT_TLS_NTP_TIMEOUT_S` (default
30 s), we **still start the listener** but log a `WARN` and bump
`$SYS/broker/tls/clock_unsynced` — the cert just has an epoch-0
`notBefore` which most clients accept with a warning. This keeps
air-gapped deployments working.

---

## Handshake worker (why not inline)

A single bad client doing an RSA handshake can stall the accept loop for
hundreds of ms. Pattern:

```c
// mqtts_listener_task
while (running) {
    int fd = accept(s_mqtts_fd, ...);
    if (fd < 0) continue;
    handshake_job_t *j = malloc(sizeof *j);
    j->fd = fd;  j->cfg = &s_tls_cfg;
    if (xQueueSend(s_hs_queue, &j, 0) != pdTRUE) {
        // queue full → 503-equivalent, drop
        close(fd); free(j);
    }
}

// mqtts_handshake_task (×2, prio = listener-1)
for (;;) {
    handshake_job_t *j;
    xQueueReceive(s_hs_queue, &j, portMAX_DELAY);
    esp_tls_t *tls = esp_tls_init();
    if (esp_tls_server_session_create(j->cfg, j->fd, tls) == 0) {
        mqtt_broker_attach_tls_client(j->fd, tls);  // hand off
    } else {
        esp_tls_conn_destroy(tls);
        close(j->fd);
    }
    free(j);
}
```

Queue depth = 4. Two handshake workers at 4 KB stack each. Total cost:
**~10 KB RAM + ~32 KB peak during a handshake** (mbedTLS scratch).

---

## Broker integration (`mqtt_broker.c`)

Add to `mqtt_client_t`:

```c
esp_tls_t *tls;          // NULL for plain clients
bool       is_tls;
char       peer_cn[64];  // populated when mTLS verified, else ""
```

Two thin wrappers:

```c
static int client_read(mqtt_client_t *c, void *buf, size_t n) {
    return c->is_tls ? esp_tls_conn_read(c->tls, buf, n)
                     : recv(c->fd, buf, n, 0);
}
static int client_write(mqtt_client_t *c, const void *buf, size_t n) {
    return c->is_tls ? esp_tls_conn_write(c->tls, buf, n)
                     : send(c->fd, buf, n, 0);
}
```

Every existing `recv(c->fd, ...)` / `send(c->fd, ...)` call site swaps to
these wrappers — grep shows **6 call sites** in `mqtt_broker.c`, all in
the per-client RX/TX paths. `select()` keeps working because `esp_tls`
exposes the underlying fd via `esp_tls_get_conn_sockfd()`.

**mTLS → username mapping.** In the CONNECT handler, before the existing
`MQTT_BROKER_AUTH_USERNAME` check (mqtt_broker.c:746):

```c
if (c->is_tls && c->peer_cn[0] && s_cfg.cn_as_user) {
    // Override: the verified cert IS the identity.
    conn.has_username = true;
    strncpy(conn.username, c->peer_cn, sizeof(conn.username) - 1);
    conn.username_len = strlen(conn.username);
    // password ignored when mTLS provides identity
}
```

This means: existing ACLs, `/clients` table, `$SYS/clients/<id>/username`
all continue to work — they just see the CN.

---

## Portal UX (extends `plan-mqtt-ux-v2.md`)

New page **Settings → Security** (or new top-level **TLS** tab — see
question below). Sections:

1. **Listener**
   - `[ ] Enable TLS on port [8883]`
   - Min version: `( ) TLS 1.2  ( ) TLS 1.3 only`
   - Status line: `Listening · 3 active TLS clients · cert expires 2035-…`

2. **Server certificate**
   - Read-only display: CN, SAN list, SHA-256 fingerprint, validity dates.
   - `[Download .crt]` button (cert only, never key).
   - `[Regenerate self-signed]` button → confirm modal → 5-second
     reboot countdown (reusing `plan-mqtt-ux-v2.md` countdown component).
   - `[Upload .crt + .key]` — drag-drop, client-side PEM sniff before
     POST, server-side `mbedtls_x509_crt_parse` validates.

3. **Client certificates (mTLS)**
   - `[ ] Require client certificate`
   - `[ ] Use cert CN as MQTT username`
   - CA bundle: textarea + `[Upload CA bundle]`. Shows count of trusted
     CAs after parse.
   - Helper: `[Generate device cert…]` opens a modal that issues a
     P-256 cert signed by an on-device CA (CA created on demand, key
     stays on device) and offers `.crt + .key + ca.crt` zip download.
     This is the "plug-it-in" path — users never touch openssl.

All POSTs are CSRF-protected via existing `csrf.c` machinery.

---

## `$SYS` topics (additive)

| Topic                              | Type   | Notes                        |
| ---------------------------------- | ------ | ---------------------------- |
| `$SYS/broker/tls/enabled`          | bool   | Mirrors NVS.                 |
| `$SYS/broker/tls/clients`          | int    | TLS-only client count.       |
| `$SYS/broker/tls/handshakes/ok`    | uint   | Lifetime success counter.    |
| `$SYS/broker/tls/handshakes/fail`  | uint   | Lifetime fail counter.       |
| `$SYS/broker/tls/cert/fingerprint` | string | Hex SHA-256, retained.       |
| `$SYS/broker/tls/cert/notAfter`    | int    | Epoch seconds, retained.     |
| `$SYS/broker/tls/clock_unsynced`   | bool   | True if started without NTP. |

---

## Kconfig

Extend `main/Kconfig.projbuild`:

```kconfig
menu "MQTT TLS"
    config MQTT_TLS_ENABLE_DEFAULT
        bool "Enable TLS listener by default"
        default n
    config MQTT_TLS_PORT_DEFAULT
        int "Default MQTTS port"
        default 8883
    config MQTT_TLS_MAX_CLIENTS
        int "Max concurrent TLS clients"
        range 1 50
        default 25
        help
          Each TLS session costs ~18 KB RAM. ESP32-S3 with 8 MB PSRAM
          can support ~50; without PSRAM keep ≤ 15.
    config MQTT_TLS_HANDSHAKE_WORKERS
        int "Handshake worker tasks"
        range 1 4
        default 2
endmenu
```

`sdkconfig.defaults` stays at `MQTT_TLS_ENABLE_DEFAULT=n` so the v0.6.x
plaintext UX is unchanged on upgrade.

---

## Memory budget (ESP32-S3, 8 MB PSRAM build)

| Item                                  | RAM         |
| ------------------------------------- | ----------- |
| `esp_tls_t` per session (idle)        | ~18 KB      |
| mbedTLS handshake scratch (transient) | ~32 KB      |
| 2 handshake workers (stacks + queue)  | ~10 KB      |
| Listener task (4 KB stack)            | ~4 KB       |
| NVS blobs cached at startup           | ~16 KB      |
| **Floor at 25 TLS clients idle**      | **~480 KB** |

Fits comfortably in PSRAM. For **no-PSRAM boards** we cap
`MQTT_TLS_MAX_CLIENTS` at 15 in Kconfig and document it in `README.md`.

---

## Testing (extends `test_broker.py` / `stress_test.py`)

New `test_tls.py` using `paho-mqtt` with `ssl.create_default_context`:

1. **Plain still works** — connect on 1883 with TLS off, then with TLS on.
2. **Self-signed accept** — connect on 8883 with
   `tls_insecure_set(True)`, verify CONNACK, pub/sub round-trip.
3. **Fingerprint pinning** — fetch `/api/tls/cert.pem`, pin its SHA-256,
   reconnect with pinning, assert success; mutate one byte → assert fail.
4. **mTLS happy path** — portal generates client cert via API, save
   to disk, connect with `tls_set(certfile=..., keyfile=...)`, assert
   `$SYS/clients/<id>/username == cert CN`.
5. **mTLS reject** — same but with a cert signed by an unrelated CA →
   handshake fails, `$SYS/broker/tls/handshakes/fail` increments.
6. **Cert rotation** — POST new cert, assert existing connection stays
   up until close, new connection sees new fingerprint.
7. **Handshake DoS** — fire 50 concurrent TLS connects via `asyncio` +
   `aiomqtt`; assert ≥ 25 succeed, no plain-1883 clients are starved
   (latency on a side `mosquitto_pub` stays < 200 ms p99).

`broker_health.py` gets a `--tls / --cafile / --cert / --key` flag set.

---

## Milestones

| #   | Scope                                                                | LOC est |
| --- | -------------------------------------------------------------------- | ------- |
| 1   | NVS schema + Kconfig + `tls_cfg.c` load/save + self-signed bootstrap | ~600    |
| 2   | `mqtts_listener` + handshake workers + read/write wrappers in broker | ~500    |
| 3   | mTLS verify + CN→username mapping + `$SYS` counters                  | ~250    |
| 4   | Portal **Security** page (HTML + JS + REST endpoints + CSRF)         | ~700    |
| 5   | On-device CA + device-cert generator + zip download                  | ~400    |
| 6   | `test_tls.py` + CI matrix (TLS on / off / mTLS) + README + CHANGELOG | ~300    |
| 7   | Stress + memory profiling on no-PSRAM board, doc memory caveats      | —       |

Each milestone is independently shippable — milestones 1–2 alone deliver
"MQTTS works out of the box".

---

## Risks / open questions

1. **RAM on no-PSRAM ESP32-S3.** 15 TLS clients may still be tight with
   the portal active. Mitigation: cap at 10 + document.
2. **Cert key in NVS, plaintext.** A flash-dump attacker gets the server
   key. For v1 we accept this and document it. v2 = HMAC-wrap with eFuse
   key (see Future Work).
3. **Time travel.** A device that boots offline, generates a cert at
   epoch 0, then later syncs NTP, will serve a cert that looks valid.
   Clients with strict clocks will refuse it until reboot. Mitigation:
   on first NTP sync after a "clock_unsynced" boot, **automatically
   regenerate** the self-signed cert if its `notBefore` < 2020-01-01.
4. **Mosquitto-compat ALPN.** Some clients send `mqtt` ALPN. mbedTLS
   accepts unknown ALPN gracefully — verified in milestone 6 test.

## Future work (out of scope here)

- HSM / eFuse-bound server key (ESP32-S3 HMAC peripheral).
- ACME / Let's Encrypt for devices with public DNS names.
- TLS for the HTTP portal (HTTPS) — separate plan, shares cert lifecycle.
- TLS-PSK fallback for ultra-low-RAM clients.

---

## Open questions for you

1. **Where does the UI live?** New top-level **TLS** tab, or a section
   inside an existing **Settings → Security** page?
2. **mTLS default username source** — CN, or SAN URI (`spiffe://…` style)?
3. **Should milestone 1 ship alone** (TLS works, no portal UI yet —
   config via CLI/NVS only), or block on the portal page being ready?
4. **Cap for no-PSRAM boards** — hard refuse to enable TLS, or allow
   with a `MQTT_TLS_MAX_CLIENTS=10` warning?
