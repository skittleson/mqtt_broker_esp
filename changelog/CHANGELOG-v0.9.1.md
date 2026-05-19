# mqtt_broker v0.9.1 — AP enable/disable toggle on /settings

User-controlled checkbox to keep the SoftAP off while the device is on
WiFi. Survives reboots, applies live, can't brick the device.

## Why

Until now the captive-portal AP came up unconditionally as soon as the
STA connection succeeded (`wifi_set_ap_mode(1)` in `main.c` on boot).
Operators with a managed WiFi network don't want a second SSID floating
around forever — they only want the AP for first-time provisioning and
recovery.

## What changed

### NVS

- New key: `mqtt_cfg/ap_enabled` (u8, default `1`). Default preserves
  pre-0.9.1 behavior — existing devices upgrading via OTA see no change.

### Portal

- `/settings` (form): new checkbox at the top of the **Access Point**
  fieldset, "Enable AP (Wi-Fi hotspot)". Includes a help paragraph
  explaining the STA-fallback safety net.
- `/save-settings` (POST): parses the checkbox (absent → 0, present →
  1), persists to NVS, and applies live via `wifi_set_ap_mode()` when
  STA is currently connected. No reboot required for the live change.
- `/information` (read-only): new **Enabled** row in the Access Point
  table — green "yes" or amber "no (auto-enabled if STA fails at
  boot)".

### Boot path (`main.c`)

- The unconditional `wifi_set_ap_mode(1)` after STA connect is now
  conditional on `mqtt_cfg/ap_enabled`.
- **Brick safety**: `wifi_connect_sta()` already brings up the AP as a
  fallback when STA can't connect at boot. That path is unchanged and
  ignores `ap_enabled`. So even if the user disables the AP and then
  later changes WiFi credentials that don't work, the device boots back
  into AP mode and the portal is reachable.

## CSRF

The checkbox rides the existing `/save-settings` form, so it's
covered by the same `csrf_verify()` gate as every other setting.
Verified: missing-CSRF POST returns HTTP 403.

## Flash impact

| Tag    | `mqtt_broker.bin` |
| ------ | ----------------- |
| 0.9.0  | n/a (local)       |
| 0.9.1  | 0x13c8c0 bytes    |

The diff over 0.9.0 for the actual feature is < 1 KB (one snprintf
fieldset, one `if (strstr())` parse arm, one NVS key). The larger
binary baseline reflects 0.9.0's Berry runtime, not this change.

## Verification trail

Live device: `http://192.168.22.100/` (Ethernet-only Waveshare
ESP32-S3-ETH, STA not connected).

```text
GET /settings           → checkbox renders with `checked` (default ON)
POST /save-settings     → no ap_enabled in body → /information shows
                          "Enabled: no (auto-enabled if STA fails at boot)"
POST /save-settings     → ap_enabled=1 in body → /information shows
                          "Enabled: yes"
POST /save-settings     → no CSRF → HTTP 403
```

## Out of scope / backlog

- **Per-event reactivation**: today the AP can only be re-enabled by
  saving /settings (or a STA failure at boot). No UX trigger like a
  button hold or MQTT command.
- **Captive-portal DNS / HTTP teardown**: when AP is off, the captive
  portal task and DNS hijack remain running with nothing to bind to.
  Cheap (a few KB RAM) and not worth the complexity per the design
  question answered up-front.
