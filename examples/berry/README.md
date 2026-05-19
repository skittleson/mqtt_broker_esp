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
| [`tasmota_power_state.be`](tasmota_power_state.be) | Subscribes to a Tasmota device's power state topic and prints ON/OFF whenever it changes. Sends a query on boot so the current state appears immediately in the log. |

## Writing your own

- Scripts run in a shared Berry VM. Globals defined in slot 0 are visible in slot 1, etc.
- `mqtt.subscribe(filter, fn)` — register a callback; `fn(topic, payload)` fires on match.
- `mqtt.publish(topic, payload [, qos [, retain]])` — publish through the broker.
- `mqtt.unsubscribe(filter)` — remove a subscription.
- `print(...)` — output appears in the `/berry` log pane (auto-refreshes every 2 s).
- Scripts have access to Berry's built-in `json`, `math`, `string`, `global` modules.
- The `http` module (async GET/POST) is planned for a future release.
