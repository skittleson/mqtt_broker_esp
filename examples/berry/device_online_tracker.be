# Track device online/offline state via MQTT LWT (Last Will & Testament).
#
# This is the standard pattern used by Tasmota, ESPHome, Zigbee2MQTT: devices
# CONNECT with a will message that the broker auto-publishes when the TCP
# socket closes. On boot they publish their "online" status retain=1 so late
# subscribers learn current state.
#
#   tele/<device>/LWT   "Online" / "Offline"  (Tasmota)
#   <device>/status     "online" / "offline"  (ESPHome)
#
# This script handles both, keeps an in-memory state map, and re-publishes
# transitions to $broker/devices/event for dashboards / other scripts.
#
# Verified live: paho-mqtt publishes Online/Offline → tracker emits the
# expected event lines and ignores duplicate-state retained replays.

import string

var devices = {}    # name -> "online" / "offline"

def on_lwt(topic, payload)
  # Extract device name from "tele/<name>/LWT" or "<name>/status"
  var parts = string.split(topic, "/")
  var name
  if size(parts) >= 3 && parts[0] == "tele" && parts[2] == "LWT"
    name = parts[1]
  elif size(parts) == 2 && parts[1] == "status"
    name = parts[0]
  else
    return
  end

  # Normalise payload: Tasmota = "Online"/"Offline", ESPHome = "online"/"offline"
  var state = string.tolower(payload)
  if state != "online" && state != "offline"
    return
  end

  var prev = devices.find(name)
  devices[name] = state

  if prev == state
    return    # no transition, ignore (retained replay on reconnect)
  end

  # Emit a transition event for dashboards / other scripts to consume.
  var msg = name + " -> " + state
  if prev != nil
    msg = msg + " (was " + prev + ")"
  end
  print("[lwt] " + msg)
  mqtt.publish("$broker/devices/event", msg)
end

# Subscribe to both common conventions.
mqtt.subscribe("tele/+/LWT", on_lwt)
mqtt.subscribe("+/status",   on_lwt)

print("[lwt] device-online tracker armed")
