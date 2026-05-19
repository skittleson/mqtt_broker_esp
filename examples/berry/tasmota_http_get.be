# tasmota_http_get.be — query a Tasmota device's power state via HTTP GET
#
# Demonstrates:
#   - http.get(url) returning [status_code, body_string]
#   - json.load() to parse the response body
#   - Printing plain-text or parsed fields
#
# Supported return formats from http module:
#   r[0]  integer HTTP status code (200 = OK, -1 = network/timeout error)
#   r[1]  string response body — treat as plain text or parse with json.load()
#
# Tested against: DVES_9ED31C (192.168.22.238), broker at 192.168.22.100
#
# To adapt: change DEVICE_IP to your Tasmota device's IP address.

var DEVICE_IP = "192.168.22.238"

print("Querying Tasmota at " + DEVICE_IP + " ...")

var r = http.get("http://" + DEVICE_IP + "/cm?cmnd=Power")

if r[0] == 200
  # Parse the JSON body: {"POWER":"ON"} or {"POWER":"OFF"}
  import json
  var state = json.load(r[1])
  print("Power state: " + str(state["POWER"]))
else
  # r[0] == -1 means network/timeout error; r[1] has the error description
  print("Request failed: " + r[1])
end
