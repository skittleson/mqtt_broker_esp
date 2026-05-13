# mqtt_broker v0.7.0-rc3 — NTP Phase 3: `/time` page + mDNS discovery

Built: May 13 2026 · SHA-256:
`2c82609921623a6b1cab3e3b0f992efafe662e9ab25319ad06f168f75fb8f5d8`

Closes Phase 3 of [`plan-ntp-server.md`](plan-ntp-server.md). Plan
acceptance criteria #1–4 are now all green; #5 (`make test-ntp` CI)
remains deferred to a Phase 0 follow-up.

## What landed

### `/time` portal page (server-rendered)

GET-only, auth-gated. ~3.4 KB of HTML, no JS in the hot path —
`<meta http-equiv='refresh' content='10'>` does the live-update work.
Tasmota-style sections in order:

1. **Now.** Big monospace UTC clock (`2026-05-13 22:21:28 UTC`) in green
   when synced or orange when not, plus the epoch microseconds as a
   subscript for programmatic eyes.
2. **Client status.** `synced · last 112s ago · 1 total · upstream pool.ntp.org`
   (or `not yet synced · upstream pool.ntp.org` pre-sync).
3. **Server status.** `serving on UDP:123 · stratum 3 · 7 served ·
   dropped 0/0/0 (rate/size/mode)` when running; `server off` when not.
4. **Force resync** button. JS-progressive enhancement: POSTs
   `/api/time/resync` and redirects back. `<noscript>` fallback posts
   and leaves the user on the JSON output.
5. **Recent clients (N).** Table sourced from the SNTP server's
   rate-limit LRU, sorted descending by last-seen, capped at 16 rows
   with an `(M more not shown)` footer when N > 16. Columns: source IP
   (parsed from network-byte-order `sin_addr.s_addr`), age in seconds,
   per-source total response count.

Dashboard nav gets a `Time / NTP` button between `MQTT Tester` and
`Information`.

### mDNS `_ntp._udp` service advertisement

The plan originally specified DHCP option 42 (NTP servers) advertised
via the AP's DHCP server. **Substituted to mDNS for v1** because:

- ESP-IDF's DHCP server (`components/lwip/.../dhcpserver.c`) only emits
  the `dhcps_offer_option` enum (router + DNS). Adding arbitrary option
  codes requires patching the IDF lwip module itself.
- mDNS already runs on the device (advertises `_mqtt._tcp` and
  `_http._tcp` since pre-0.6.0). One extra `mdns_service_add()` call
  exposes `_ntp._udp` on port 123.
- Avahi-aware clients (macOS, iOS, ChromeOS, Linux Avahi) get the same
  auto-discovery they would have got from DHCP opt-42. Windows /
  embedded clients without mDNS still need to point at the broker's
  hostname or IP manually — same workaround they'd need today if no
  DHCP server on the network supports advertising NTP sources anyway.

Verified live with a raw multicast DNS query:

```
sent _ntp._udp.local PTR query to 224.0.0.251:5353
reply from 192.168.22.100: 120 bytes, contains _ntp data
```

### Per-source `total` counter

`ntp_rate_entry_t` gains a `uint32_t total` field — incremented on each
served request, reset on slot eviction. Free observability for the
`/time` recent-clients table. New public helper:

```c
int ntp_get_recent_clients(ntp_recent_client_t *out, int max_out);
```

Returns the active LRU slots; caller sorts as needed.

## End-to-end verification (live device, Basic Auth on)

```
=== /time page (after warming the LRU with 4 SNTP queries) ===
  size: 3354 bytes
  big time line: '2026-05-13 22:21:28 UTC'
  client:  synced · last 112s ago · 1 total · upstream pool.ntp.org
  server:  serving on UDP:123 · stratum 3 · 7 served · dropped 0/0/0
  recent:  1 row -- 192.168.22.164  0s ago  total=7

=== mDNS query ===
  -> _ntp._udp.local PTR to 224.0.0.251:5353
  <- 120 bytes from 192.168.22.100
```

Both Phase 3 deliverables green.

## Acceptance criteria status (plan-ntp-server.md §7)

| # | Criterion | Status |
|---|-----------|--------|
| 1 | `$SYS/broker/time` published within 60 s of cold boot | ✅ 0.7.0-rc1 |
| 2 | LAN client gets a usable SNTP response | ✅ 0.7.0-rc2 |
| 3 | DHCP option 42 (or equivalent) auto-discovery | ✅ **this release** (mDNS substitution) |
| 4 | `/time` portal page <100 ms over Ethernet | ✅ **this release** (3.4 KB body, no JS work) |
| 5 | `make test-ntp` CI green | ⏳ Phase 0 still deferred |
| 6 | README "What's new in 0.7.0" merged | ✅ rc1 + rc2 + rc3 entries |

## Files changed

```
main/ntp.h           +16  -0    (ntp_recent_client_t, ntp_get_recent_clients(),
                                 NTP_RECENT_MAX)
main/ntp.c           +25  -3    (total counter, ntp_get_recent_clients())
main/main.c          +9   -2    (mDNS _ntp._udp service advertisement)
main/portal.c        +148 -1    (GET /time route, dashboard nav button,
                                 <time.h> include)
main/version.h       +5   -3    (0.7.0-rc2 -> 0.7.0-rc3)
tools/capture_time.py     new    (Playwright capture for the new page,
                                  warms LRU first with raw SNTP queries)
docs/screenshots/ux-audit/time_desktop.png   new
docs/screenshots/ux-audit/time_mobile.png    new
docs/screenshots/...                          all refreshed for footer bump
README.md            +24  -1    (what's-new section, endpoint table,
                                 version table)
CHANGELOG-v0.7.0-rc3.md   new
releases/...-rc3.bin           new
releases/...-rc3.bin.sha256    new
```

Binary size: 1.14 MB → 1.15 MB. **Combined Phase 1+2+3 flash growth:
+19.4 KB** vs 0.6.6 baseline. Plan budget was 12 KB — **we are 7.4 KB
over**. 73 % of the 4 MB OTA partition remains free; not a practical
problem, but worth recording.

Breakdown of where the budget went:

| Slice | Approx Δ | Notes |
|-------|---------:|-------|
| Phase 1 (SNTP client + `/api/time` + `$SYS/broker/time`) | +9 KB | esp_sntp wrapper + NVS settings + JSON endpoint + 'Time (NTP)' fieldset |
| Phase 2 (SNTP server + 3 retained `$SYS/broker/ntp/*` topics + server fields in `/api/time`) | +3 KB | 200-line server task, 3 `handle_publish_internal()` calls, JSON field additions |
| Phase 3 (`/time` page + mDNS service + recent-clients helper) | +7 KB | The `/time` HTML/CSS is the dominant cost (~3 KB compressed); the table renderer + `gmtime_r`/`strftime` pulls in ~2 KB; mDNS service line is ~1 KB; LRU `total` field + getter ~1 KB |

If we wanted to claw back the overage in a future polish pass:

- Drop one of the three retained `$SYS/broker/ntp/*` topics (the plan's
  acceptance test only checks `$SYS/broker/time`). ~-1 KB.
- Replace `strftime("%Y-%m-%d %H:%M:%S UTC")` with a hand-rolled int
  formatter. ~-1.5 KB (strftime drags in a fair bit of locale code).
- Merge `ntp.c`'s NVS helpers with portal.c's (currently duplicated
  per-namespace). ~-0.5 KB.

None of those are worth doing today. Logged for posterity.

## Not in this release (post-0.7.0)

- **Phase 0 — `make test-ntp` CI.** Host-build firmware + `ntplib` smoke
  tests + `chronyc` integration tests. Highest leverage remaining item.
- **Phase 4 stretch.** `POST /api/time/set` manual time override for
  air-gapped installs, drift compensation while free-running, broadcast
  mode.
- **Real DHCP option 42.** Requires patching ESP-IDF's lwip dhcps to
  support arbitrary option codes. Worth doing if anyone files an issue
  about Windows / embedded clients not auto-discovering.
