<div align="center">

# ESP32 MQTT Broker

**A standalone MQTT 3.1.1 broker that runs entirely on a $10 microcontroller.**
No cloud. No Pi. No Docker. Plug it in.

[![ESP-IDF v5.5](https://img.shields.io/badge/ESP--IDF-v5.5-blue?style=flat-square)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
[![MQTT 3.1.1](https://img.shields.io/badge/MQTT-3.1.1-orange?style=flat-square)](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)
![QoS 0/1](https://img.shields.io/badge/QoS-0%20%2F%201-green?style=flat-square)
![100 clients](https://img.shields.io/badge/clients-100%20max-green?style=flat-square)
[![License: MIT](https://img.shields.io/badge/license-MIT-brightgreen?style=flat-square)](LICENSE)
[![Stars](https://img.shields.io/github/stars/skittleson/mqtt_broker_esp.svg?style=flat-square)](https://github.com/skittleson/mqtt_broker_esp/stargazers)

[Quick start](#quick-start) · [Features](#features) · [Docs](#documentation) · [Building](#building-from-source) · [Testing](#testing)

<img src="docs/screenshots/dashboard.png" alt="Web portal dashboard" width="720">

</div>

---

Most MQTT setups need a Raspberry Pi, a cloud account, or a Linux box running
Mosquitto. This puts the **entire broker on an ESP32-S3**: 100 concurrent
clients, QoS 0/1, retained messages, OTA updates, a Tasmota-style web portal,
and an SNTP server — all on an 8 MB chip you can power from a USB battery.

Built for home automation, IoT sensor fleets, and edge deployments that need a
local broker with zero maintenance.

## Quick start

```bash
# Build & flash (requires ESP-IDF v5.5+)
source $IDF_PATH/export.sh
git clone https://github.com/skittleson/mqtt_broker_esp.git
cd mqtt_broker_esp
idf.py build flash monitor
```

On first boot the device comes up as a WiFi AP:

1. Connect to **`mqtt-broker`** (password `mqtt1234`)
2. Open <http://192.168.25.1>, enter your WiFi credentials
3. After reboot it joins your network and starts the broker on port **1883**
4. Discover it: `avahi-browse -rt _mqtt._tcp` → it advertises as `mqtt_broker.local`

```bash
mosquitto_sub -h mqtt_broker.local -t "test/#" -v &
mosquitto_pub -h mqtt_broker.local -t "test/hello" -m "world"
```

## Features

- **Full MQTT 3.1.1** — CONNECT, SUBSCRIBE, PUBLISH, PUBACK, UNSUBSCRIBE, PINGREQ, DISCONNECT
- **QoS 0 and QoS 1** in both directions, with in-flight retry tables and `min(pub, granted)` delivery
- **100 concurrent clients**, 2,048 subscriptions, 16 KB payloads (binary-safe up to 64 KB retained)
- **Retained messages** with configurable TTL, PSRAM-backed, FIFO eviction at 80% PSRAM
- **Wildcards** — `+`, `#`, and `$SYS` topic protection per spec §4.7
- **Authentication** — optional MQTT username/password + Basic Auth on the portal
- **Web portal** — Tasmota-style dark UI: dashboard, live `/clients`, settings, OTA, firmware rollback
- **OTA updates** — file upload, URL fetch, dual partitions, manual rollback button
- **Built-in NTP** — SNTP client + SNTPv4 server on UDP :123, mDNS `_ntp._udp` advertisement, <±50 ms drift
- **Ethernet gateway mode** (optional) — W5500 SPI + NAPT to bridge an isolated IoT WiFi to your LAN
- **mDNS** — reachable as `<hostname>.local`, advertises `_mqtt._tcp`, `_http._tcp`, `_ntp._udp`
- **WS2812 status LED** — visual boot/connect/run/AP state
- **No external MQTT library** — single C codebase, the only non-IDF dep is `espressif/led_strip`

<details>
<summary>Use cases</summary>

- **Home automation hub** — local MQTT for Zigbee/Z-Wave bridges, ESPHome, Home Assistant
- **Mobile / field** — USB battery + ESP32 = portable MQTT infra for trade shows, ag, testing
- **Network isolation** — Ethernet build bridges a 2.4 GHz IoT WiFi to your LAN with togglable NAPT
- **Client tracking** — `/api/clients` exposes every connected device for dashboards/alerts

</details>

## Hardware

| Component | Spec                                                                                               |
| --------- | -------------------------------------------------------------------------------------------------- |
| Board     | [Waveshare ESP32-S3-ETH](https://www.waveshare.com/wiki/ESP32-S3-ETH) (or any ESP32-S3 with PSRAM) |
| MCU       | ESP32-S3 dual-core Xtensa LX7 @ 240 MHz                                                            |
| PSRAM     | 8 MB octal SPI                                                                                     |
| Flash     | 16 MB (dual 4 MB OTA partitions)                                                                   |
| WiFi      | 802.11 b/g/n 2.4 GHz                                                                               |
| LED       | WS2812 on GPIO21                                                                                   |
| Console   | USB-Serial/JTAG                                                                                    |

Any ESP32-S3 board with PSRAM works. The W5500 on the Waveshare board is only
needed for [Ethernet gateway mode](docs/architecture.md#ethernet-gateway-w5500).

## Web portal

<p align="center">
  <img src="docs/screenshots/clients.png" alt="Live clients page" width="380">
  &nbsp;
  <img src="docs/screenshots/settings.png" alt="Settings page" width="380">
</p>

- **`/`** — dashboard with WiFi/broker stats, MQTT auth state, device info
- **`/clients`** — live MQTT clients (ID, IP, uptime, subs, in-flight, published, keepalive) + WiFi AP clients (MAC, RSSI). Polls `/api/clients` every 3 s, pause button, tab-hidden backoff
- **`/settings`** — broker port, auth, buffer size, retain, AP credentials, hostname, NAPT, NTP. **Save & Reboot** with confirm dialog and countdown page
- **`/update`** — file upload, URL fetch, rollback button showing the other partition's version
- **`/time`** — live clock, NTP client+server status, recent SNTP clients, force-resync
- **JSON API** — `/api/status`, `/api/clients`, `/api/time`, `/api/ping` (open, for liveness)

Full endpoint and JSON reference: [`docs/api.md`](docs/api.md).

## Configuration

All settings persist to NVS and survive reboots.

| Setting           | Default                    | Notes                                                |
| ----------------- | -------------------------- | ---------------------------------------------------- |
| MQTT port         | 1883                       | 1–65535, takes effect after reboot                   |
| Auth user/pass    | _(empty)_                  | Empty = open broker                                  |
| Buffer size       | 16,384                     | 1,024–65,536, per-client recv + shared send          |
| Retain enable     | on                         | Off rejects all retain flags                         |
| Retain TTL        | 168 h                      | 0 = never expire                                     |
| AP SSID / pass    | `mqtt-broker` / `mqtt1234` | WPA2-PSK                                             |
| AP IP             | `192.168.25.1`             | Also compile-time                                    |
| Hostname          | `mqtt_broker`              | DHCP + mDNS (`<hostname>.local`)                     |
| NAPT              | on                         | Ethernet builds only, toggles live                   |
| NTP client/server | on / on                    | Up to 3 upstreams, configurable poll interval and TZ |

Compile-time tunables (max clients, in-flight slots, retry timing, LED GPIO,
SPI pins, …) live in `main/mqtt_broker.h`, `main/Kconfig.projbuild`, and
`sdkconfig.defaults`. See [docs/architecture.md](docs/architecture.md#scaling-the-client-limit)
for scaling past 100 clients.

## Building from source

```bash
# ESP-IDF v5.5+: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/
source $IDF_PATH/export.sh

idf.py build              # build
idf.py flash monitor      # flash + serial log
idf.py menuconfig         # tweak: "MQTT Broker Configuration"
```

For the Ethernet gateway build (W5500 + NAPT):

```bash
cat sdkconfig.defaults sdkconfig.defaults.eth > sdkconfig.combined
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.combined" reconfigure
idf.py build
```

## Testing

The test suite runs against a **live broker** (there is no host-build today —
tests target the real radio + Ethernet stack on a flashed device).

```bash
pip install paho-mqtt requests ntplib jsonschema

# Everything (MQTT + NTP), default host 192.168.22.100
make test

# Targeted
BROKER_HOST=192.168.1.100 make test-broker        # 116 MQTT/portal tests
BROKER_HOST=192.168.1.100 make test-ntp           # 13 NTP tests

# With portal auth
BROKER_AUTH=admin:secret make test

# Destructive cycle (saves settings, reboots 2-3×, ~2 min extra)
BROKER_TEST_DESTRUCTIVE=1 make test

# Stress: 90 concurrent connections, 500-msg throughput, 255 topics
python3 stress_test.py
```

**~129 assertions** cover: protocol conformance, wildcards, retained, binary
payloads up to 15 KB with MD5 verification, 50-client concurrency, throughput,
latency, duplicate client IDs, keep-alive, QoS-1 inbound + outbound, unsubscribe,
all portal pages and JSON APIs, settings persistence, NTP client+server, mDNS
discovery, anti-amplification, and rate-limit drops.

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — tasks, cores, memory layout, flash partitions, QoS internals, Ethernet/NAPT, LED states, network modes, scaling past 100 clients
- [`docs/api.md`](docs/api.md) — all HTTP endpoints, JSON schemas, `$SYS` topics, curl examples
- [`docs/portal-latency-analysis.md`](docs/portal-latency-analysis.md) — measurements behind the 0.6.6 latency work
- [`docs/qos-persistence-plan.md`](docs/qos-persistence-plan.md) — roadmap for persistent sessions
- [`CHANGELOG.md`](CHANGELOG.md) — per-release notes (full text under [`changelog/`](changelog/))

## Roadmap

- QoS 2
- Persistent sessions in PSRAM (no per-message flash writes — 10-year device-life goal)
- TLS / certificate auth
- Drift compensation while free-running
- Manual `POST /api/time/set` for air-gapped installs
- Bootloader auto-rollback with an in-app self-test

## Contributing

PRs welcome. Workflow:

1. Fork & branch (`git checkout -b feature/x`)
2. Build (`idf.py build`) and run `make test` against your device
3. Open a PR with the test output

## Acknowledgments

Web portal UX takes cues from the [Tasmota](https://tasmota.github.io/docs/)
project — the dark theme, full-width button menus, fieldset-based info
sections, and Information/Configuration/Firmware Upgrade split. Different
goal (broker, not device firmware), same taste in layout.

## License

[MIT](LICENSE). Custom MQTT implementation — no external broker library. Only
non-IDF dependency is `espressif/led_strip` for the WS2812 status LED.
