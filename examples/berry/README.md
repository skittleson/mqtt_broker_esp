# Berry script examples

Ready-to-paste Berry scripts for common broker automation tasks.
Copy the contents of any `.be` file into a slot on the `/berry` page.

## How to use

1. Open the broker portal → **Berry scripting** (`/berry`).
2. Click **Edit** on an empty slot.
3. Give it a name, paste the script, check **Enable**, click **Save & restart**.
4. The script runs on every broker boot. Output appears in the **Log** pane.

## Examples

| File | What it does |
|------|--------------|
| [`tasmota_power_state.be`](tasmota_power_state.be) | Subscribes to a Tasmota device's MQTT power state topic and prints ON/OFF whenever it changes. Sends a query on boot so the current state appears immediately in the log. |
| [`tasmota_http_get.be`](tasmota_http_get.be) | Queries a Tasmota device's power state via HTTP GET, parses the JSON response body, and prints the result. Demonstrates the two supported response formats: JSON and plain text. |

## API quick reference

### mqtt module

```berry
mqtt.subscribe("sensor/+", def(topic, payload)
  print(topic + " -> " + payload)
end)

mqtt.publish("my/topic", "hello")          # QoS 0, not retained
mqtt.publish("my/topic", "hello", 1, true) # QoS 1, retained

mqtt.unsubscribe("sensor/+")
```

### http module

```berry
# GET — returns [status_code, body_string]
var r = http.get("http://192.168.1.1/api")
print(r[0])   # 200
print(r[1])   # {"result":"ok"}

# POST — returns [status_code, body_string]
var r2 = http.post("http://host/path", "body text")
var r3 = http.post("http://host/path", '{"key":"val"}', "application/json")

# Optional timeout (milliseconds, default 5000)
var r4 = http.get("http://slow-host/", 10000)

# Error case: r[0] == -1, r[1] has the error description
if r[0] == -1
  print("Error: " + r[1])
end
```

**Supported response formats:** JSON (parse with `json.load(r[1])`) or plain text
(`r[1]` as-is). No binary, no XML.

### Built-in modules

```berry
import json
var obj = json.load('{"key":"value"}')
print(str(obj["key"]))     # value

import string
print(string.count("hello", "l"))  # 2

import math
print(math.sqrt(16))       # 4.0
```

### print()

`print(...)` writes to the `/berry` log pane (auto-refreshes every 2 s).

## Writing your own

- Scripts run in a shared Berry VM. Globals defined in slot 0 are visible in slots 1–3.
- All enabled slots run in order (slot 0 first) on every broker boot/restart.
- The `http` module blocks berry_task for the duration of each request — keep
  timeouts short (default 5 s) and avoid looping requests in autoexec scripts.
- The `http` module (P4) and `mqtt` module (P3) are available out of the box.
  Planned future modules: `watchdog`, `kv`, `log`.
