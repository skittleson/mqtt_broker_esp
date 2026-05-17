# Architecture

## Tasks & cores

The broker is a single FreeRTOS task pinned to Core 1 using a `select()` loop
for non-blocking I/O across all client sockets. Core 0 runs WiFi and the
HTTP/DNS portal tasks.

```
app_main()
  ├── NVS init
  ├── LED init + led_task              (Core 0, 2 KB stack)
  ├── WiFi STA connect                 (blocks up to 60s; AP fallback on failure)
  ├── wifi_set_ap_mode(1)              →  AP+STA mode
  ├── portal_start()
  │     ├── portal_http_task            (Core 0, 12 KB stack, :80)
  │     └── portal_dns_task             (Core 0, 12 KB stack, :53)
  ├── ntp_start()                      (Core 0, SNTP client + UDP :123 server)
  └── broker_start()
        └── broker_task                (Core 1, 16 KB stack)
              ├── Load NVS config (port, auth, retain, buffers)
              ├── Allocate clients[100], recv buffers, subs[2048],
              │   inflight[100][20], send buffer  (all PSRAM)
              ├── Bind TCP :1883
              └── select() loop
                    ├── accept() new clients
                    ├── recv() → parse MQTT → route
                    ├── Keep-alive enforcement      (every 5 s)
                    ├── Stats logging               (every 30 s)
                    └── Retained message expiry
```

## Memory layout (8 MB PSRAM)

| Allocation            | Size            | Notes                                                                  |
| --------------------- | --------------- | ---------------------------------------------------------------------- |
| Client structs        | ~10 KB          | 100 × `broker_client_t` (without recv buf)                             |
| Recv buffers          | 1,600 KB        | 100 × 16 KB (configurable)                                             |
| Subscription pool     | 280 KB          | 2,048 × `broker_sub_t`                                                 |
| Send buffer           | 16 KB           | Shared, configurable                                                   |
| QoS-1 in-flight slots | ~96 KB          | 100 × 20 × `broker_inflight_t` (headers only; payloads on-demand)      |
| QoS-1 in-flight bytes | up to 2 MB      | Dynamic topic+payload allocs, cap enforced; overflow degrades to QoS 0 |
| Retained messages     | up to ~5,120 KB | 80% of remaining PSRAM                                                 |
| **Free heap**         | ~6,250 KB       | Available for retained store + general use                             |

## Flash layout (16 MB)

| Partition | Offset   | Size  | Purpose                          |
| --------- | -------- | ----- | -------------------------------- |
| nvs       | 0x9000   | 24 KB | Settings, WiFi credentials, auth |
| otadata   | 0xF000   | 8 KB  | OTA boot selection               |
| ota_0     | 0x20000  | 4 MB  | App slot A                       |
| ota_1     | 0x420000 | 4 MB  | App slot B                       |

OTA updates alternate between ota_0 and ota_1. The running partition is never
overwritten. Manual rollback only — bootloader auto-rollback is intentionally
off until in-app self-test is wired up (roadmap).

## MQTT QoS

QoS 0 and QoS 1 are supported in both directions. Per-subscriber delivery is
`min(publisher_qos, granted_qos)` per [MQTT-3.3.1-9].

**Inbound QoS 1** (publisher → broker): broker accepts the PUBLISH, fans it
out, then PUBACKs the publisher [MQTT-4.3.2-2]. Ownership transfers at PUBACK
— the broker does not wait for subscriber delivery before acking.

**Outbound QoS 1** (broker → subscriber): each PUBLISH is held in an in-flight
slot until PUBACK arrives. Missing acks are retransmitted with `DUP=1`.

| Setting                  | Default  | Notes                                                                      |
| ------------------------ | -------- | -------------------------------------------------------------------------- |
| In-flight slots / client | 20       | Overflow → degrade to QoS 0 (still delivered)                              |
| Global memory cap        | 2 MB     | Total in-flight topic+payload bytes; overflow also degrades to QoS 0       |
| First retry              | 15 s     | After original send                                                        |
| Retry backoff            | ×2       | 15 s, 30 s, 60 s, 60 s, 60 s                                               |
| Max retries              | 5        | Then abandoned with a log line                                             |
| Persistence              | RAM only | Reboot drops all queues. Persistent sessions on roadmap (PSRAM, not flash) |

QoS 2 is **not implemented**. Inbound QoS-2 PUBLISH packets are dropped
silently (publisher retries with `DUP=1`); SUBACK clamps granted QoS ≥ 2 → 1.

## Source layout

```
main/
├── main.c            Entry point: NVS, WiFi, LED, portal, broker startup
├── version.h         Firmware version defines (semver + name)
├── mqtt_broker.h/c   MQTT core: select() loop, clients/subs, QoS-1 retry tables
├── mqtt_parser.h/c   MQTT 3.1.1 packet parser/serializer, topic matching
├── portal.h/c        HTTP server, DNS hijack, settings UI, JSON API, OTA handlers
├── ntp.h/c           SNTP client (esp_sntp) + UDP:123 server
├── eth_connect.h/c   W5500 SPI init, DHCP, NAPT bridging (compile-time flag)
└── wifi_connect.h/c  WiFi STA/AP, NVS persistence, portal callbacks
```

## Scaling the client limit

The broker ships configured for **100 concurrent MQTT clients**. Three knobs
have to stay in agreement:

| Setting                      | Default | Where                              | Rule                                          |
| ---------------------------- | ------- | ---------------------------------- | --------------------------------------------- |
| `MQTT_BROKER_MAX_CLIENTS`    | 100     | `main/mqtt_broker.h`               | Broker-level cap on accepted MQTT clients     |
| `CONFIG_LWIP_MAX_SOCKETS`    | 115     | `sdkconfig` / `sdkconfig.defaults` | ≥ max_clients + ~15 (listener + portal + DNS) |
| `CONFIG_LWIP_MAX_ACTIVE_TCP` | 115     | `sdkconfig` / `sdkconfig.defaults` | Same as `MAX_SOCKETS`                         |

The ~15 headroom covers: 1 MQTT listen socket, the HTTP portal (~5 concurrent),
DNS hijack, mDNS, OTA HTTP fetch, and short-lived accept/close transitions.

Per added client: ~16 KB PSRAM recv buffer, ~100 B internal struct,
**~2 KB lwIP overhead in DRAM** — that's the tight constraint. **150 clients is
the realistic ceiling** on ESP32-S3 with 8 MB PSRAM before DRAM (not PSRAM)
for lwIP runs out. Past that, run two brokers and partition devices.

```bash
# 1. main/mqtt_broker.h
#    #define MQTT_BROKER_MAX_CLIENTS 150
# 2. sdkconfig (or menuconfig → Component config → LWIP)
#    CONFIG_LWIP_MAX_SOCKETS=165
#    CONFIG_LWIP_MAX_ACTIVE_TCP=165
idf.py fullclean && idf.py build flash
```

If you forget step 2, new clients silently fail to connect once the lwIP pool
is exhausted — even though the broker still has free MQTT slots.

## Ethernet gateway (W5500)

With `CONFIG_MQTT_BROKER_ETHERNET=y`, the ESP32 bridges a wired LAN to its
WiFi AP subnet using NAPT.

```
[Your LAN / PC]                    [Tasmota Device A: 192.168.25.2]
      |                                      |
  [Ethernet / W5500]              [WiFi AP: 192.168.25.1/24]
      |                                      |
      +----------[ ESP32-S3 MQTT Broker ]----+
                   NAPT bridges the two subnets
```

IoT devices on `192.168.25.x` are hardware-isolated from your main network.
MQTT pub/sub flows through the broker regardless. NAPT only matters when you
need direct IP-level access — opening a Tasmota web UI, pushing an OTA, hitting
a device's HTTP endpoint, ping. Toggle NAPT at runtime from `/settings` (NVS
persisted).

**SPI pins** (configurable via `idf.py menuconfig` → MQTT Broker Configuration):

| Signal | Default GPIO | Kconfig key           |
| ------ | ------------ | --------------------- |
| MOSI   | 11           | `CONFIG_ETH_SPI_MOSI` |
| MISO   | 13           | `CONFIG_ETH_SPI_MISO` |
| SCLK   | 12           | `CONFIG_ETH_SPI_SCLK` |
| CS     | 10           | `CONFIG_ETH_SPI_CS`   |
| INT    | 4            | `CONFIG_ETH_SPI_INT`  |
| RST    | 5            | `CONFIG_ETH_SPI_RST`  |

**Building:**

```bash
cat sdkconfig.defaults sdkconfig.defaults.eth > sdkconfig.combined
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.combined" reconfigure
idf.py build
```

## LED status

| Pattern    | Color  | Meaning                                           |
| ---------- | ------ | ------------------------------------------------- |
| Fast blink | Blue   | Booting                                           |
| 2-blink    | Yellow | Connecting to WiFi                                |
| 3-blink    | Red    | WiFi failed, AP mode active                       |
| Slow pulse | Green  | WiFi connected, broker running                    |
| Slow pulse | Cyan   | AP-only mode, portal running                      |
| Slow pulse | White  | Ethernet gateway mode (W5500 connected + WiFi AP) |

## Network modes

| Mode        | When                                     | Broker              | Portal            |
| ----------- | ---------------------------------------- | ------------------- | ----------------- |
| **STA**     | Connected to WiFi, AP disabled           | `<WiFi IP>:1883`    | `<WiFi IP>:80`    |
| **AP+STA**  | Connected to WiFi, AP enabled (default)  | `<WiFi IP>:1883`    | Both IPs on `:80` |
| **AP only** | No WiFi credentials or connection failed | `192.168.25.1:1883` | `192.168.25.1:80` |
