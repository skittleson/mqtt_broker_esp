# MQTT Tester UX — Deployment Runbook (v0.3.0)

This is the operator checklist for shipping the MQTT tester to **http://192.168.22.100/**.

Code changes are complete and built. See `plan-mqtt-ux.md` for the full plan, and the diff summary at the end of this file.

## Build artifacts

| File                         | Size        | SHA-256                                                            |
| ---------------------------- | ----------- | ------------------------------------------------------------------ |
| `releases/tester-v0.3.0.bin` | 1,114,288 B | `2a28a203342b8bd86d3835e182cf9b5ef774e7d75b63ddf2d7d21cf9e5b37d8c` |

Binary growth vs. tester-disabled build: **+8,960 bytes** (well under the 30 KB ceiling).

## What you still must do BEFORE OTA (sections 1, 6.5/6.6 of the plan)

### 1. Produce rollback.bin from the firmware currently running on the device

The current firmware on `192.168.22.100` is **whatever was last built/flashed before today's changes**. I do not have that binary in this checkout. Build it from the matching git commit and archive it:

```bash
cd /home/spencerkittleson/repos/mqtt_esp32
git stash                              # stash today's changes
git log -1 --format='%H %s'            # confirm we're on the pre-tester commit
source ~/lvgl_micropython/lib/esp-idf/export.sh
idf.py fullclean && idf.py build
cp build/mqtt_broker.bin releases/rollback.bin
sha256sum releases/rollback.bin | tee releases/rollback.bin.sha256
cp releases/rollback.bin ~/Dropbox/   # or wherever your off-laptop copy lives
git stash pop                          # restore today's changes
idf.py fullclean && idf.py build       # rebuild the deployable
cp build/mqtt_broker.bin releases/tester-v0.3.0.bin
```

Verify `releases/tester-v0.3.0.bin` SHA matches the table above after rebuild.

### 2. Confirm live device firmware version

```bash
curl -u admin:<password> http://192.168.22.100/information | grep -i version
```

Record: `current version = ________` , confirm it matches what `rollback.bin` was built from.

### 3. Snapshot current state for post-deploy comparison

```bash
curl -u admin:<password> http://192.168.22.100/clients > /tmp/clients-before.html
curl -u admin:<password> http://192.168.22.100/api/status > /tmp/status-before.json
cat /tmp/status-before.json | python3 -m json.tool
```

Record: clients = `____` , retained = `____` , free_heap = `____`.

### 4. Dry-run the OTA path with the CURRENT firmware (plan §6.6)

This validates `/ota-upload` works today before you have something new to roll back to.

```bash
# Re-upload the existing firmware (no behavior change, just proves the path)
curl -u admin:<password> -F "firmware=@releases/rollback.bin" \
     http://192.168.22.100/ota-upload
```

Watch for the success page + reboot. After ~30 s, hit `/information` again. **If this fails, do NOT proceed — the OTA path is the only safety net.**

### 5. Record physical recovery owner

Where is `192.168.22.100`? Who has USB access? Record here: `__________________`

## Deployment (plan §8)

```bash
# Open in two terminals before clicking upload:
# Terminal A (canary subscriber — will reconnect after reboot):
mosquitto_sub -h 192.168.22.100 -u <mqtt_user> -P <mqtt_pass> -t '#' -v

# Terminal B (ready to rollback):
# curl -u admin:<pw> -F "firmware=@releases/rollback.bin" http://192.168.22.100/ota-upload

# Now upload the new build:
curl -u admin:<password> -F "firmware=@releases/tester-v0.3.0.bin" \
     http://192.168.22.100/ota-upload
```

**Wait window: 30–90 seconds.**

- ✅ Terminal A reconnects and resumes printing messages → broker is alive.
- ✅ `curl http://192.168.22.100/information` shows version `0.3.0`.
- ✅ `curl http://192.168.22.100/tester` returns HTML (auth gated).
- ❌ Terminal A stays disconnected after 90 s → execute rollback in Terminal B.
- ❌ Portal unreachable → physical USB recovery.

## Smoke test (plan §8.7–8.8)

1. Open `http://192.168.22.100/tester` in a browser. Confirm the "connected" badge turns green within 2 s.
2. From terminal: `mosquitto_pub -h 192.168.22.100 -u <u> -P <p> -t hello -m "from-cli"`. Confirm the message appears in the browser within 1 s.
3. In the tester UI: type topic=`tester/hello`, payload=`from-ui`, click Publish. Confirm Terminal A (mosquitto_sub) prints it.
4. Open a **second** browser tab to `/tester`. Confirm both tabs see messages.
5. Open a **third** tab → WS connection should be refused (server says `503 Service Unavailable`; check browser devtools network tab). Close third tab.
6. Close one of the first two tabs. After a few seconds confirm broker_tester slot count returns to 1 (`curl http://192.168.22.100/api/status` if exposed, or just open a fresh third tab and confirm it works now).

## 10-minute soak (plan §8.9)

Leave it running. Watch:

- Terminal A still receiving real-client traffic.
- `curl http://192.168.22.100/api/status` every minute → free_heap stable, clients count unchanged from baseline.

## Rollback (plan §9)

```bash
curl -u admin:<password> -F "firmware=@releases/rollback.bin" \
     http://192.168.22.100/ota-upload
```

Reboots within ~10 s, returns to the previous version.

## Code diff summary

```
 main/CMakeLists.txt |   4 +-     # added portal_ws.c, mbedtls require
 main/mqtt_broker.c  | 303 ++++   # tester registry, queue, fanout hook (all #ifdef'd)
 main/mqtt_broker.h  |  81 +++    # tester API declarations
 main/portal.c       | 130 +++    # /ws upgrade detection + /tester page + nav link
 main/portal_ws.c    | 715 +++    # NEW — WebSocket framing, JSON, per-client task
 main/portal_ws.h    |  35 +++    # NEW
 main/version.h      |   6 +-     # 0.2.1 -> 0.3.0
```

Hot paths reviewed (plan §7.6):

1. **Broker fanout hook** (`handle_publish_internal` → `tester_fanout` in `mqtt_broker.c`): non-blocking, runs only after the existing subscriber fanout, drops on full stream buffer.
2. **WS frame parser** (`recv_frame` in `portal_ws.c`): rejects fragmented frames, rejects 64-bit lengths, caps payload at 2 KB, requires masking.
3. **Cross-task queue** (`broker_tester_request_publish` → `tester_drain_publish_queue`): bounded drain (max 8/iteration), zero-timeout xQueueSend, never blocks the broker.

## Known follow-ups (post-deploy)

- Binary payload support (hex input).
- Server-side topic filtering (currently always subscribed to `#`, filter is client-side JS).
- Heap-usage metrics on the tester page.
- **Acquire a bench ESP32-S3 before the next change of this size** (plan §10.4).
