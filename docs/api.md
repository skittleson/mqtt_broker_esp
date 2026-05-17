# HTTP & MQTT API

## HTTP endpoints

| Path               | Method | Auth  | Description                                                                      |
| ------------------ | ------ | ----- | -------------------------------------------------------------------------------- |
| `/`                | GET    | gated | Dashboard with live stats                                                        |
| `/clients`         | GET    | gated | Connected MQTT + WiFi AP clients (live, in-place refresh)                        |
| `/settings`        | GET    | gated | Broker / AP / NTP settings form                                                  |
| `/config`          | GET    | gated | WiFi configuration form                                                          |
| `/update`          | GET    | gated | Firmware update page (upload + URL + rollback)                                   |
| `/time`            | GET    | gated | Live clock, NTP client+server status, force-resync                               |
| `/rebooting`       | GET    | open  | Standalone reboot countdown page (read-only, no reboot)                          |
| `/api/ping`        | GET    | open  | Liveness — `{uptime_s}`. Bypasses Basic Auth so countdown polling won't prompt.  |
| `/api/status`      | GET    | gated | Broker stats, WiFi, firmware version, system info                                |
| `/api/clients`     | GET    | gated | Connected MQTT + WiFi AP clients                                                 |
| `/api/time`        | GET    | open  | `{synced, epoch_us, last_sync_age_s, sync_count, upstream, server_running, ...}` |
| `/api/time/resync` | POST   | gated | Force an immediate upstream NTP poll                                             |
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
    "version": "0.7.0",
    "build": "Apr 28 2026 12:55:31"
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
```
