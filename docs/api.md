# HTTP & MQTT API

## HTTP endpoints

| Path               | Method | Auth  | Description                                                                      |
| ------------------ | ------ | ----- | -------------------------------------------------------------------------------- |
| `/`                | GET    | gated | Dashboard with live stats                                                        |
| `/clients`         | GET    | gated | Connected MQTT + WiFi AP clients (live, in-place refresh)                        |
| `/timers`          | GET    | gated | List of 16 scheduled-publish slots + master pause                                |
| `/timers/edit`     | GET    | gated | Per-slot edit form (query `?n=1..16`). `&saved=1` shows the saved banner         |
| `/timers/save`     | POST   | gated | Persist a slot from the edit form; redirects to `/timers/edit?n=&saved=1`        |
| `/timers/clear`    | POST   | gated | Wipe a slot to defaults                                                          |
| `/timers/master`   | POST   | gated | Toggle master pause (`enable=0\|1`)                                              |
| `/timers/fire`     | POST   | gated | Test-fire one slot immediately, irrespective of schedule                         |
| `/settings`        | GET    | gated | Broker / AP / NTP settings form (includes the timezone preset dropdown)          |
| `/config`          | GET    | gated | WiFi configuration form                                                          |
| `/update`          | GET    | gated | Firmware update page (upload + URL + rollback)                                   |
| `/time`            | GET    | gated | Live clock, NTP client+server status, force-resync                               |
| `/rebooting`       | GET    | open  | Standalone reboot countdown page (read-only, no reboot)                          |
| `/api/ping`        | GET    | open  | Liveness — `{uptime_s}`. Bypasses Basic Auth so countdown polling won't prompt.  |
| `/api/status`      | GET    | gated | Broker stats, WiFi, firmware version, system info                                |
| `/api/clients`     | GET    | gated | Connected MQTT + WiFi AP clients                                                 |
| `/api/time`        | GET    | open  | `{synced, epoch_us, last_sync_age_s, sync_count, upstream, server_running, ...}` |
| `/api/time/resync` | POST   | gated | Force an immediate upstream NTP poll                                             |
| `/api/timers`      | GET    | gated | All 16 slots + master state + `next_fire_unix` per slot                          |
| `/api/timers/<n>`  | PUT    | gated | Replace slot `<n>` (1..16) from JSON body. Validates; returns `next_fire_unix`. CSRF required. |
| `/api/timers/<n>`  | DELETE | gated | Wipe slot `<n>`. CSRF required.                                                  |
| `/ota-upload`      | POST   | gated | OTA firmware upload (multipart/form-data)                                        |
| `/ota-url`         | POST   | gated | OTA firmware fetch from URL (`http://` or `https://`)                            |
| `/ota-rollback`    | POST   | gated | Switch boot partition to the other OTA slot and reboot                           |
| `/save-settings`   | POST   | gated | Save broker / AP / NTP settings to NVS                                           |
| `/save`            | POST   | gated | Save WiFi credentials                                                            |
| `/clear`           | GET    | gated | Clear saved WiFi credentials                                                     |
| `/reconnect`       | GET    | gated | Reconnect to saved WiFi                                                          |
| `/ap-toggle`       | GET    | gated | Toggle AP mode                                                                   |
| `/reboot`          | GET    | gated | Reboot the device                                                                |

## Network services

| Service     | Port     | Notes                                                                                             |
| ----------- | -------- | ------------------------------------------------------------------------------------------------- |
| MQTT broker | TCP 1883 | MQTT 3.1.1, QoS 0/1, up to 100 clients                                                            |
| HTTP portal | TCP 80   | Web UI + JSON API                                                                                 |
| Captive DNS | UDP 53   | Hijacks DNS in AP mode for first-boot setup                                                       |
| SNTP server | UDP 123  | SNTPv4. Stratum 16/LI=3 pre-sync, stratum 3 post-sync. Per-source rate limit, anti-amplification. |
| mDNS        | UDP 5353 | Advertises `_mqtt._tcp:1883`, `_http._tcp:80`, `_ntp._udp:123`                                    |

## JSON: `GET /api/status`

```json
{
  "wifi": {
    "connected": true,
    "ssid": "MyNetwork",
    "ip": "192.168.1.100",
    "ap": true
  },
  "broker": {
    "clients": 12,
    "max_clients": 100,
    "subs": 47,
    "retained": 3,
    "retained_kb": 1,
    "port": 1883
  },
  "firmware": {
    "name": "mqtt_broker",
    "version": "0.8.2",
    "build": "May 18 2026 12:43:00"
  },
  "system": { "uptime_s": 86400, "free_heap_kb": 6300 }
}
```

## JSON: `GET /api/clients`

```json
{
  "mqtt": [
    {
      "client_id": "sensor-kitchen",
      "ip": "192.168.8.42",
      "connected_s": 3600,
      "last_active_s": 2,
      "subs": 3,
      "inflight": 0,
      "published": 142,
      "keep_alive": 60
    }
  ],
  "wifi_ap": [{ "mac": "AA:BB:CC:DD:EE:01", "rssi": -45 }]
}
```

## JSON: `GET /api/timers`

```json
{
  "schema": 1,
  "master": true,
  "now_unix": 1779129423,
  "dropped": 0,
  "timers": [
    {
      "n": 1,
      "arm": true,
      "repeat": true,
      "retain": false,
      "qos": 0,
      "window": 0,
      "time": "17:00",
      "days": "SMTWTFS",
      "topic": "cmnd/tasmotas/POWER",
      "label": "All lights ON",
      "payload": "ON",
      "payload_len": 2,
      "next_fire_unix": 1779210000
    }
    /* + 15 more slots (empty ones have arm=false / topic="" / next_fire_unix=0) */
  ]
}
```

`time` is `HH:MM` 24-hour in the device's POSIX TZ (`NVS "ntp"/"tz"`, set on
`/settings`). `days` is a 7-char mask where position 0..6 = Sun..Sat;
`-`/`0` = off, any other character = on. `next_fire_unix` is computed
via `localtime_r`/`mktime` so DST transitions are honoured automatically.

## JSON: `PUT /api/timers/<n>`

Body: one slot object. Accepts compact wire keys (`a`, `r`, `rt`, `q`,
`w`, `hm`, `d`, `tp`, `pl`, `l`) **or** long-form (`arm`, `repeat`,
`retain`, `qos`, `window`, `time` or `min`, `days`, `topic`, `payload`,
`label`). Headers: `X-CSRF-Token: <token from /api/csrf>`,
`Content-Type: application/json`.

Valid request:

```bash
curl -u admin:secret \
  -H "X-CSRF-Token: $TOKEN" \
  -H 'Content-Type: application/json' \
  -X PUT http://broker.local/api/timers/1 \
  -d '{"a":1,"r":1,"hm":1020,"d":"-MTWTF-",
       "tp":"cmnd/tasmotas/POWER","pl":"ON","l":"lights on"}'
```

Response 200:

```json
{ "saved": true, "n": 1, "next_fire_unix": 1779210000 }
```

Validation failures return HTTP 400 with `{"error":"<message>"}` —
examples: `topic required when armed`, `time must be 00:00..23:59`,
`invalid topic (wildcards, $SYS, or control chars not allowed)`.

## JSON: `DELETE /api/timers/<n>`

Wipes slot `<n>` to defaults (`arm=false`, all fields empty). Requires
CSRF.

```bash
curl -u admin:secret -H "X-CSRF-Token: $TOKEN" \
     -X DELETE http://broker.local/api/timers/3
# → {"cleared":true,"n":3}
```

## `$SYS` topics

| Topic                     | Retained | Notes                           |
| ------------------------- | -------- | ------------------------------- |
| `$SYS/broker/time`        | no       | ASCII epoch seconds, every 10 s |
| `$SYS/broker/ntp/synced`  | yes      | `0` / `1`                       |
| `$SYS/broker/ntp/stratum` | yes      | Current stratum (16 = unsynced) |
| `$SYS/broker/ntp/served`  | yes      | SNTP requests served counter    |

## curl examples

```bash
# Liveness
curl http://broker.local/api/ping

# Broker / system status
curl -u admin:secret http://broker.local/api/status

# OTA upload (~20 s for 1.1 MB over Ethernet)
curl -u admin:secret -F "firmware=@build/mqtt_broker.bin" http://broker.local/ota-upload

# OTA from URL
curl -u admin:secret -X POST -d "url=http://host/mqtt_broker.bin" http://broker.local/ota-url

# Rollback to other partition
curl -u admin:secret -X POST http://broker.local/ota-rollback

# Force NTP resync
curl -u admin:secret -X POST http://broker.local/api/time/resync

# List all timer slots (incl. next_fire_unix per slot)
curl -u admin:secret http://broker.local/api/timers

# Schedule a daily 17:00 weekday publish via JSON API
TOKEN=$(curl -s -u admin:secret http://broker.local/api/csrf | jq -r .token)
curl -u admin:secret -H "X-CSRF-Token: $TOKEN" \
     -H 'Content-Type: application/json' \
     -X PUT http://broker.local/api/timers/1 \
     -d '{"a":1,"r":1,"hm":1020,"d":"-MTWTF-",
          "tp":"cmnd/tasmotas/POWER","pl":"ON","l":"lights on"}'

# Clear a timer slot
curl -u admin:secret -H "X-CSRF-Token: $TOKEN" \
     -X DELETE http://broker.local/api/timers/1
```
