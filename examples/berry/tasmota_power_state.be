# tasmota_power_state.be — print power state of a Tasmota device
#
# What it does:
#   1. Subscribes to the Tasmota stat topic for POWER responses.
#   2. Publishes an empty payload to the cmnd topic, which asks Tasmota
#      to report its current power state without changing it.
#   3. The callback fires when Tasmota replies and prints the state.
#      The subscription stays live: any subsequent power state change
#      (button, MQTT command, schedule) also triggers a print.
#
# Tested against: Waveshare ESP32-S3-ETH broker at 192.168.22.100
#                 Tasmota device at 192.168.22.238 (client DVES_9ED31C)
#
# Tasmota MQTT topic layout (default FullTopic = %prefix%/%topic%/):
#   cmnd/<topic>/Power  — send "" to query, "ON"/"OFF"/"TOGGLE" to set
#   stat/<topic>/POWER  — Tasmota replies here
#   tele/<topic>/STATE  — periodic telemetry (also contains POWER field)
#
# To adapt for your device:
#   1. Find your Tasmota's MQTT topic:
#        curl "http://<device-ip>/cm?cmnd=Topic"
#      returns e.g. {"Topic":"tasmota_9ED31C"}
#   2. Replace "tasmota_9ED31C" below with your topic value.

var TOPIC = "tasmota_9ED31C"

mqtt.subscribe("stat/" + TOPIC + "/POWER", def(topic, payload)
  print("Tasmota power state: " + payload)
end)

mqtt.publish("cmnd/" + TOPIC + "/Power", "")
print("Power query sent to " + TOPIC + ", waiting for response...")
