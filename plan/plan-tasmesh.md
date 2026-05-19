# Plan: Tasmesh broker support (v0.8.0 target)

Same structure as [`plan-ntp-server.md`](plan-ntp-server.md): scope,
phases with acceptance criteria, risks and decisions captured before the
first line of code. **No code is written from this plan yet** — the
purpose is to record what we know and what we'd commit to.

## TL;DR

Make the broker speak Tasmota's **Tasmesh** ESP-NOW protocol so that any
existing `#define USE_TASMESH` Tasmota node can attach without firmware
changes on the node side, appearing as a normal MQTT client in `/clients`
and `/api/clients`. **Wire-compatible with `arendst/Tasmota` development
branch as of the `xdrv_57_9` revision dated 2024-Q1 (post PR #20392
HEARTBEAT addition).**

Five phases over ~10 working days, gated on green Phase-0 tests at every
step. Estimated footprint: +25–35 KB binary, ~8 KB resident heap, one
new FreeRTOS task pinned to CPU 1. Comfortable on our current device
(73 % OTA slot free, 6.2 MB free heap).

## Why ship this

1. **Battery sensors at the edge of WiFi.** ESP-NOW reaches further at
   lower power than 802.11. Existing Tasmota nodes can flip to mesh
   mode and continue reporting through our broker without joining the
   2.4 GHz network at all. Useful at this site for outbuilding sensors
   that currently drop out.

2. **Symmetry with the broker's purpose.** We're already the MQTT
   broker. Being the *mesh broker* too means there's exactly one
   "thing" on the LAN for Tasmota devices to point at, regardless of
   whether they connect over WiFi-MQTT or ESP-NOW. Reduces deployment
   surface.

3. **Free piggyback on the NTP work.** Tasmesh broadcasts UTC every
   ~30 s as `PACKET_TYPE_TIME`. Our SNTP server already produces
   authoritative wall-clock time. Plumbing that into `MESHsendTime()`
   is a few lines.

4. **No new hardware.** WiFi STA is unused on this Ethernet-primary
   device today — that radio is sitting idle behind the SoftAP rescue
   path. Tasmesh uses it.

## Decisions recorded up front

| # | Decision | Rationale |
|---|---|---|
| **D1** | **Strategy A — SoftAP-only WiFi** for the radio. WiFi STA permanently disabled when mesh is enabled; SoftAP and ESP-NOW share a single fixed channel chosen in `/settings` (default: 6). | We're Ethernet-primary. STA isn't doing anything that justifies the channel-hop bug class. Tasmota's own docs flag STA-follow as a recurring pain point ("rule on system#boot to re-issue MeshChannel"). |
| **D2** | **Wire-compatible only.** No proprietary extensions, no extra packet types beyond the 8 currently defined (`TIME`, `PEERLIST`, `COMMAND`, `REGISTER_NODE`, `REFRESH_NODE`, `MQTT`, `WANTTOPIC`, `HEARTBEAT`). | Lets any Tasmota node attach with no firmware change. Pinning to Tasmota's current header layout makes us a passive consumer of their protocol; tracking PR #20392-style additions is mechanical. |
| **D3** | **ChaCha20-Poly1305 key = dedicated NVS field `mesh.psk`,** *not* the SoftAP password and *not* the WiFi STA password. Settings UI shows it masked, save-and-reboot to apply. First 32 bytes used (zero-padded if shorter). Min 8 chars, max 32 chars. | Tasmota uses `WiFiPassword1`. We'd inherit that, but the SoftAP password is currently `mqtt1234` factory default and gets shown to anyone who can read the AP fallback. Decoupling makes the threat model less surprising and lets us rotate the mesh key without breaking WiFi clients. |
| **D4** | **Broker MAC = SoftAP MAC** of our device. We publish it on the dashboard, on `/mesh`, and via mDNS `_tasmesh._udp` so users can scrape it for the node-side `MeshNode XX:XX:XX:XX:XX:XX` command. | Matches Tasmota's own docs ("WARNING: The MAC address used for ESP-NOW on the broker is the Soft AP MAC, not the WiFi MAC"). |
| **D5** | **Topic namespacing on injected MQTT:** a node with Tasmota topic `tasmota_AB12CD` publishes its tele/stat/cmnd payloads as-is. We do **not** prefix `mesh/`. From the rest of the LAN's point of view, mesh nodes are indistinguishable from WiFi-attached Tasmota clients. | The whole point: zero migration cost. Home Assistant etc. don't need to learn a new topic shape. |
| **D6** | **Mesh clients appear in `/clients` with `mesh:<MAC>` as `client_id`** and `transport=esp_now` so they're visually distinct. They count against `max_clients` like any other. | One unified table; users find their nodes where they expect. |
| **D7** | **No mesh forwarding (TTL=1 always).** Star topology only. Nodes that need a 2-hop path can use Tasmota's `MeshPeer` on their own — we don't have to participate in node-to-node forwarding. | Drops a whole class of dedup/loop-prevention complexity. We can add mesh-of-mesh later if any user asks. |
| **D8** | **Mesh enable defaults to OFF** in fresh NVS. Existing 0.7.0 deployments are unaffected by the OTA. | No surprises. |

## Phase 0 — Test harness (done first, on every reroll)

Same discipline as `plan-ntp-server.md` Phase 0. The wrinkle here is
that **we can't simulate ESP-NOW frames from the dev host** — they're
802.11 vendor-specific action frames that need a real WiFi radio in the
right mode. Plan accordingly.

### What we'd build

| File | What it does |
|---|---|
| `test_tasmesh.py` | Host-side tests that drive the **observable surface**: HTTP `/api/mesh` (peer table), `/clients` filter for `transport=esp_now`, retained `$SYS/broker/mesh/{peers,packets_rx,packets_tx,drops}`. Does **not** generate ESP-NOW frames. |
| `tools/fake_mesh_node.py` | Cross-compiles a tiny ESP-IDF firmware for a **second ESP32** that acts as a synthetic Tasmesh node: REGISTER_NODE on boot, periodic MQTT publish, heartbeat. Used by the gate test below. |
| `tools/mesh_sniffer.py` | Runs on a **third ESP32 in promiscuous WiFi mode**, capturing ESP-NOW action frames on the chosen channel and forwarding parsed packet headers over USB serial. Lets us verify wire format from a known-good Tasmota node alongside ours. Optional in CI, used in development. |
| Makefile additions | `make test-mesh` runs `test_tasmesh.py` against the live broker. Requires `FAKE_MESH_NODE=1` env to spin up the second ESP32. |

### Acceptance criteria (Phase 0)

| # | Criterion | How verified |
|---|---|---|
| 0.1 | Synthetic node firmware builds and flashes | `make fake-node-build && make fake-node-flash PORT=/dev/ttyUSB1` |
| 0.2 | Sniffer firmware builds and runs on a third ESP32 | `make sniffer-build && make sniffer-monitor PORT=/dev/ttyUSB2` |
| 0.3 | `test_tasmesh.py` runs against a live 0.8.0 broker with no Phase-1+ implementation: all tests in xfail/skipped (placeholders) | `make test-mesh` exits 0, lists 12 skipped |
| 0.4 | Hand-soldered Tasmota-on-ESP32 dev node (real Tasmota build) joins a real Tasmota broker successfully when both are flashed from `arendst/Tasmota` `development`. Confirms our reference implementation works against the same hardware we'll test against. | Manual: `MeshBroker` on one, `MeshNode XX:...` on the other, verify telemetry flows. |

Phase 0 is done when 0.1–0.4 are all green. We are **not** "test-harness
broken" if Tasmota's own `development` branch changes the wire format —
that's a tracked risk, not a Phase-0 blocker. Re-verify 0.4 quarterly.

## Phase 1 — ESP-NOW receive path (read-only)

Goal: a mesh node can register with our broker and we ingest its MQTT
publish into the local broker. **No outbound, no TIME broadcast, no
peer-table cleanup.** Proof of life.

### What lands

- `main/mesh.{c,h}` — new module. Public API:
  ```c
  esp_err_t mesh_init(void);          // start ESP-NOW, register CB
  esp_err_t mesh_enable(bool on);     // toggle at runtime
  bool      mesh_is_enabled(void);
  size_t    mesh_get_peers(mesh_peer_info_t *out, size_t cap);
  void      mesh_get_stats(mesh_stats_t *s);
  ```
- ChaCha20-Poly1305 decrypt via mbedtls (already linked):
  `mbedtls_chachapoly_auth_decrypt`. Nonce is the 12 B from
  `&packet->receiver[6]` per Tasmota's choice; AAD is the 14 B header
  block before the tag. Key is `mesh.psk` zero-padded to 32 B.
- Packet layout matches `xdrv_57_1_tasmesh_support.ino`:
  ```
  struct mesh_packet {              // 196 B total
      uint8_t  sender[6];           // MAC
      uint8_t  receiver[6];         // MAC
      uint32_t bitfield;            // counter:4 | type:6 | chunks:6
                                    // chunk:6 | chunkSize:8 | TTL:2
      uint32_t senderTime;          // UTC or peerIndex
      uint8_t  tag[16];             // Poly1305 tag
      uint8_t  payload[160];        // encrypted in place
  } __attribute__((packed));
  ```
- One FreeRTOS task on CPU 1: `mesh_rx_task`, fed from
  `esp_now_recv_cb_t` via a `xQueueHandle`. Strict
  no-malloc-in-callback discipline like NTP.
- Multi-packet reassembly: bitmask over 26 chunks per peer per
  rolling-counter, ring buffer of 8 in-flight reassemblies (drops
  oldest when full). Static allocation.
- Peer table: 32 slots, MAC + topic (≤ 64 B) + `last_rx_us` +
  `last_heartbeat_us` + `alive` + `counter`. NVS namespace `mesh`
  persists only `enabled`, `channel`, `psk`. **Peers are
  not persisted** — they re-REGISTER after a broker reboot
  (Tasmota nodes already do this every ≤30 s).
- On `PACKET_TYPE_REGISTER_NODE` / `REFRESH_NODE`: read the topic from
  payload offset 6 (payload[0..5] is flags+pad), allocate peer slot,
  emit `INFO mesh: + AA:BB:CC:DD:EE:FF "tasmota_AB12CD"`.
- On `PACKET_TYPE_MQTT`: reassemble if `chunks > 1`, parse as
  `<topic>\0<payload>`, call `mqtt_broker_publish_local(topic, payload,
  qos=0, retain=0)` *as if* the node was a connected MQTT client. The
  publish appears on `$SYS/broker/messages_received` like any other.
- Defensive guards (drop counters):
  - Packet length ≠ 196 → drop
  - AEAD tag mismatch → drop, increment `dropped_crypto`
  - Unknown packet type → drop
  - Per-source rate limit: same 32-slot LRU as SNTP, 10 packets/s
  - Counter went backwards by > 8 → drop (replay)

### Acceptance criteria (Phase 1)

| # | Criterion | Test |
|---|---|---|
| 1.1 | Synthetic node sends REGISTER, broker logs the join | Watch serial log, check `/api/mesh` returns 1 peer |
| 1.2 | Synthetic node publishes `tele/fake_a/SENSOR` (180 B payload, 2 chunks) | Real MQTT client subscribed to `#` receives the message exactly once |
| 1.3 | Wrong-key node (synthetic with mismatched PSK) is silently dropped | `/api/mesh` shows `dropped_crypto > 0`, no peer added |
| 1.4 | 32 simultaneous nodes registered, table full | 33rd node REGISTER drops with `dropped_full`, first 32 still healthy |
| 1.5 | Replay of an old packet (same counter, same sender) dropped | `dropped_replay` increments |
| 1.6 | `/api/mesh` JSON schema | jsonschema validation passes |
| 1.7 | `make test-mesh` green | 7 passing, 0 failing |
| 1.8 | Portal latency p95 with 10 nodes publishing at 1 Hz | ≤ 70 ms (vs current 54 ms baseline; +16 ms budget for mesh RX work) |

## Phase 2 — Outbound MQTT and TIME broadcast

Goal: the broker can talk back. Inbound `cmnd/<node_topic>/...` on the
MQTT side is forwarded to the right node via ESP-NOW. TIME broadcasts
every 30 s and on-demand within 5 s of a `nodeWantsTime` flag.

### What lands

- Auto-subscribe: when a node REGISTERs with topic `fake_a`, the broker
  internally subscribes to `cmnd/fake_a/#`. Handled via a new
  `mqtt_broker_register_mesh_subscriber()` API in `mqtt_broker.c` that
  bypasses the network layer and invokes the publish-routing path
  directly with a synthetic client ID.
- TX path: when `cmnd/fake_a/POWER` arrives, find the peer by topic,
  encrypt `cmnd/fake_a/POWER\0ON` into one or more 160 B chunks, fire
  `esp_now_send(node_mac, ...)` with `MESH_INTERVAL_MS` between chunks
  to avoid ESP-NOW back-pressure.
- TIME broadcast: piggyback on NTP's existing `$SYS/broker/time`
  10 s tick. Once per 3 ticks (30 s), build a `PACKET_TYPE_TIME` with
  `senderTime = (uint32_t)time(NULL)`, encrypt, broadcast.
- `WANTTOPIC` demand: every 5 s, scan peer table for slots with empty
  topic but a recent counter; emit `WANTTOPIC` to that MAC.

### Acceptance criteria (Phase 2)

| # | Criterion | Test |
|---|---|---|
| 2.1 | `mosquitto_pub -t cmnd/fake_a/POWER -m ON` reaches the synthetic node | Synthetic node's "rx" UART output shows the message |
| 2.2 | Multi-chunk outbound (cmnd payload 250 B) split into 2 chunks, reassembled correctly at node | Synthetic node logs full message |
| 2.3 | TIME broadcast received by synthetic node within 30 s of join | Node's clock matches broker's within 1 s |
| 2.4 | `$SYS/broker/mesh/packets_tx` retained, increments per outbound | MQTT subscriber sees it |
| 2.5 | No outbound when mesh disabled | `mesh.enabled=0` → `cmnd/fake_a/...` silently dropped on the mesh side (still published normally) |

## Phase 3 — Heartbeat, LWT, `/mesh` portal page

Goal: full UX parity with NTP feature. Mesh visibility in `/clients`,
dedicated `/mesh` page, retained `$SYS/broker/mesh/*` topics, LWT for
nodes that fall off.

### What lands

- HEARTBEAT handling (`PACKET_TYPE_HEARTBEAT`, type 7 per PR #20392):
  update `last_heartbeat_us`. If `now - last_heartbeat > 90 s`, mark
  node offline, publish `tele/<topic>/LWT = "Offline"` retained. On
  next heartbeat, publish `Online`. Mirrors Tasmota's
  `TASMESH_OFFLINE_DELAY` (60 s default; we use 90 s for hysteresis).
- `/mesh` portal page: server-rendered (no JS hot path), meta-refresh
  10 s. Sections:
  - Mesh status card (enabled, channel, broker MAC, PSK length)
  - Peer table (MAC, topic, alive Y/N, last RX age, packet counter, last tele msg preview)
  - "Add a node" instructions block: copyable `MeshNode XX:XX:XX:XX:XX:XX` line with our broker MAC
- Dashboard gets a new **Mesh** button between **Time / NTP** and **MQTT Tester**, badge shows live peer count
- `/clients` table gets a `Transport` column (mqtt-tcp vs esp-now), already-existing rows unaffected
- Retained `$SYS/broker/mesh/{peers,packets_rx,packets_tx,dropped_size,dropped_crypto,dropped_replay,dropped_full,dropped_rate}` published every 10 s
- mDNS service `_tasmesh._udp.local` advertising the broker MAC for discovery (analogous to our `_ntp._udp`)
- Settings page gets a "Tasmesh" fieldset: enable, channel (1-13), PSK, save-and-reboot.

### Acceptance criteria (Phase 3)

| # | Criterion | Test |
|---|---|---|
| 3.1 | `/mesh` renders all four sections, body < 5 KB | `test_tasmesh.py` |
| 3.2 | Synthetic node stops heartbeating → LWT `Offline` published within 100 s | `tele/fake_a/LWT` subscriber sees the message |
| 3.3 | `$SYS/broker/mesh/*` schema | jsonschema validation |
| 3.4 | mDNS query `_tasmesh._udp.local` returns broker MAC | `avahi-browse -r _tasmesh._udp` |
| 3.5 | Settings save-and-reboot picks up new PSK; old PSK nodes drop with `dropped_crypto` | Manual + test |

## Phase 4 — Hardening and release

- Power: ESP-NOW RX in light sleep evaluation (battery sensors aren't
  *us*, but if WiFi STA is gone we may be able to drop our own idle
  current).
- OTA path test: full OTA of 0.7.0 → 0.8.0 with 10 mesh nodes attached.
  Nodes must re-REGISTER within 30 s of broker boot. Document this in
  the changelog ("nodes will appear offline for ~30 s during the OTA").
- README: new "Tasmesh" section with the rationale-from-Tasmota-docs
  quote, dashboard screenshot, `/mesh` screenshot, mobile view.
- Changelog `CHANGELOG-v0.8.0.md` with all four phase audits inline,
  binary-size diff table, plan-vs-actual scorecard.
- Tag `v0.8.0`, stash binary in `releases/`.

### Acceptance criteria (Phase 4)

| # | Criterion |
|---|---|
| 4.1 | `make test` (broker + ntp + mesh) green against 0.8.0 |
| 4.2 | 10-node soak test: 24 h, 0 dropped packets above 0.1 % rate, p95 portal latency < 80 ms |
| 4.3 | OTA from 0.7.0 → 0.8.0 with 10 attached nodes; all re-register within 60 s |
| 4.4 | README screenshot carousel updated to include `/mesh` desktop + mobile |
| 4.5 | Plan scorecard 8/8 (D1-D8) all marked decided + verified |

## Wire format reference (for implementation cross-check)

Pulled from `arendst/Tasmota` `development` /
`tasmota/tasmota_xdrv_driver/xdrv_57_1_tasmesh_support.ino` at the
2024-Q1 head:

```
Packet layout (196 bytes total):
  offset  size  field
  ----------------------------------------------
   0       6    sender MAC
   6       6    receiver MAC (broadcast 0xFF*6 for many types)
  12       4    bitfield, packed little-endian:
                  bits 0..3   counter (rolling, 0..15)
                  bits 4..9   type (0..63; only 0..7 used)
                  bits 10..15 chunks (1..26 typically)
                  bits 16..21 chunk (0..chunks-1)
                  bits 22..29 chunkSize (0..160)
                  bits 30..31 TTL (0..3)
  16       4    senderTime  -- uint32 UTC seconds OR peerIndex
  20      16    Poly1305 tag
  36     160    ChaCha20-encrypted payload
                  (only chunkSize bytes are valid; rest may be junk)

AEAD parameters (BearSSL on Tasmota, mbedtls on us):
  algorithm: ChaCha20-Poly1305 (RFC 8439, single-shot)
  key:       first 32 B of PSK, zero-padded if shorter (we use NVS mesh.psk)
  nonce:     bytes [18..29] of the packet header (12 B starting at
             receiver[6], skipping the first 2 bytes of the bitfield).
             This is Tasmota's quirky choice; we match it exactly.
  AAD:       bytes [0..13] of the packet header (sender + receiver + 
             first 2 bytes of bitfield).
  tag:       16 B, stored at offset 20.

Packet types (in the 6-bit `type` field):
  0  TIME            broker -> all (broadcast). senderTime = UTC.
  1  PEERLIST        node-only (we ignore RX, never TX in star mode)
  2  COMMAND         reserved, never used in current Tasmota
  3  REGISTER_NODE   node -> broker. payload[0..5] = flags+pad,
                     payload[6..] = node's MQTT topic (NUL-terminated).
                     Triggers immediate TIME broadcast.
  4  REFRESH_NODE    node -> broker. Same shape; slightly-delayed TIME.
  5  MQTT            either direction. payload = `<topic>\0<message>`.
                     If chunks > 1, payload spans multiple packets;
                     reassemble by (sender, counter) into a
                     chunks*160-byte buffer, then parse.
  6  WANTTOPIC       broker -> specific node. Empty payload. Asks the
                     node to re-send REGISTER. We TX, never RX.
  7  HEARTBEAT       node -> broker. Empty payload. Per PR #20392 to 
                     drive MQTT LWT online/offline transitions.

Pacing (Tasmota defaults):
  send interval:  50 ms (MESH_REFRESH)
  TIME broadcast: every 30 s, or 5 s on demand
  HEARTBEAT:      every 1 s from node, broker times out at 60 s default
```

## Open questions for next session

- **Q1**: Does the SoftAP need to be "promotable" to dual-purpose
  (rescue portal + mesh) or do we accept that enabling mesh disables
  the WiFi STA path? **Current plan: D1 says yes, disable STA.** Worth
  one more sanity check.
- **Q2**: Should mesh-nodes-published topics be visible via `/api/clients`
  with full message-rate metrics (parity with TCP clients), or just
  appear in a separate `/api/mesh` payload? **Current plan: D6, both —
  unified `/clients` with a transport column.**
- **Q3**: Do we ever want to *act as a node* (forward to an upstream
  Tasmota broker)? **Plan answer: no, never. We are a broker. If a
  user wants that, they put a Tasmota ESP32 in front of us.**
- **Q4**: Sniffer-as-3rd-ESP32 in Phase 0 — practical to mandate or
  too much hardware burden? **Plan answer: optional. The test harness
  must run with just the dev host + broker + 1 synthetic node. Sniffer
  is a debug aid only.**

## Not in 0.8.0 (held for later)

- **Mesh-of-mesh forwarding** (TTL > 1). Star only.
- **PEERLIST distribution.** We accept REGISTER, never echo a peer
  list. Tasmota nodes that need a 2-hop fallback can populate
  themselves via the node-side `MeshPeer` command.
- **`COMMAND` packet type.** Tasmota itself doesn't use it; we won't
  invent semantics.
- **Custom packet types for our own telemetry** (e.g. a mesh ping).
  Wire-compat first; extensions later, opt-in.
- **Tasmesh v2 / "ESP-Mesh-Lite"** by Espressif. Different protocol.
  Out of scope.
- **Forwarding from non-Tasmota ESP-NOW devices.** Possible later via
  a separate `xdrv_57_compat` shim; not in 0.8.0.

## Risks and what we'd do about them

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Tasmota changes wire format mid-development | Medium | Rerun on the reference Tasmota build. Pin our wire-format reference to a Tasmota commit hash in `mesh.c` top comment. | Watch `arendst/Tasmota` `info/xdrv_57_tasmesh.md` diffs quarterly. |
| Portal latency regression from ESP-NOW RX IRQ pressure | Medium | High (we just earned the 54 ms p95) | Phase 1 acceptance gate 1.8 sets a hard ceiling. If we miss, RX work moves off CPU 1 main task into a higher-priority handler with bounded queue. |
| ChaCha20-Poly1305 implementation mismatch with BearSSL (different padding, nonce convention) | Low | High (no mesh works at all) | Phase 0.4 — make sure a real Tasmota node talks to our reference Tasmota broker first, then swap in our broker as the target. If our broker fails where Tasmota's succeeds, the AEAD is the bug. |
| User picks weak PSK → trivial brute force, mesh compromised | High | Medium (LAN-local attack) | Settings page enforces min 8 chars + warns if = SoftAP password. No upper-bound enforcement beyond 32 chars (matches Tasmota). |
| OTA cycle disconnects mesh nodes for 30 s | Certain | Low | Document in changelog. Nodes re-REGISTER automatically. |
| Channel conflicts with neighboring WiFi APs | Medium | Medium (degraded throughput, no data loss) | Settings page shows a "scan channels" button that runs `esp_wifi_scan_start` and reports density per channel. User picks the quietest. |

## Estimated effort

| Phase | Dev | Test | Total |
|---|---:|---:|---:|
| 0 — Test harness | 1.5 d | 0.5 d | 2.0 d |
| 1 — RX path | 2.0 d | 1.0 d | 3.0 d |
| 2 — TX + TIME | 1.5 d | 0.5 d | 2.0 d |
| 3 — UX + LWT + /mesh | 1.5 d | 0.5 d | 2.0 d |
| 4 — Release | 0.5 d | 0.5 d | 1.0 d |
| **Total** | **7.0 d** | **3.0 d** | **10.0 d** |

Each phase ends with an OTA cycle, screenshot refresh, and a `feat(mesh)`
+ `docs(mesh)` commit pair. Phase-completion ≠ tag — we tag once at 4.

## Acceptance scorecard (template, fill in per phase)

| # | Phase 0 | Phase 1 | Phase 2 | Phase 3 | Phase 4 |
|---|:-:|:-:|:-:|:-:|:-:|
| .1 | ☐ | ☐ | ☐ | ☐ | ☐ |
| .2 | ☐ | ☐ | ☐ | ☐ | ☐ |
| .3 | ☐ | ☐ | ☐ | ☐ | ☐ |
| .4 | ☐ | ☐ | ☐ | ☐ | ☐ |
| .5 | — | ☐ | ☐ | ☐ | — |
| .6 | — | ☐ | — | — | — |
| .7 | — | ☐ | — | — | — |
| .8 | — | ☐ | — | — | — |

## References

- `arendst/Tasmota` `info/xdrv_57_tasmesh.md` (driver docs)
- `arendst/Tasmota` `tasmota/tasmota_xdrv_driver/xdrv_57_1_tasmesh_support.ino` (peer/header/AEAD)
- `arendst/Tasmota` `tasmota/tasmota_xdrv_driver/xdrv_57_9_tasmesh.ino` (broker + node main loop)
- `arendst/Tasmota` PR #20392 (HEARTBEAT + LWT addition, packet type 7)
- `arendst/Tasmota` discussion #11939 (TasMesh original design rationale)
- `arendst/Tasmota` discussion #23235 (current MeshBroker command-handling gaps)
- ESP-IDF `esp_now` component reference (`components/esp_wifi/include/esp_now.h`)
- `plan-ntp-server.md` (template this plan follows)
