# ESP32-C6 LED Node — API Reference

## Overview

Each device runs two independent servers:

- **WebSocket** on port **81** — real-time LED control
- **HTTP REST** on port **80** — location mapping management and device configuration

Devices are individually addressable by mDNS hostname or IP.

```
ws://led-node-1.local:81      # WebSocket
http://led-node-1.local:80    # HTTP
```

> **Windows note:** `.local` hostnames require Bonjour. Install *Apple Bonjour Print Services* if they don't resolve, or use the IP address shown in the Serial Monitor on boot.

---

## WebSocket API

### Connection

Connect to `ws://<hostname>:81` using any standard WebSocket client.

- All messages are **UTF-8 text frames** containing a single JSON object
- Every command returns **exactly one JSON reply**
- Multiple clients can connect simultaneously

### Response format

**Success:**
```json
{ "status": "ok", "cmd": "<command_name>" }
```

**Success with extra data** (location commands):
```json
{ "status": "ok", "cmd": "location", "leds": 3 }
```

**Error:**
```json
{ "status": "error", "msg": "<description>" }
```

---

### Commands

> Any command that sets colours stops a running effect automatically.

---

#### `fill` — Set all LEDs to one colour

```json
{ "cmd": "fill", "r": 255, "g": 0, "b": 0 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"fill"` |
| `r` | integer | yes | Red, 0–255 |
| `g` | integer | yes | Green, 0–255 |
| `b` | integer | yes | Blue, 0–255 |

---

#### `set` — Set a single LED by index

```json
{ "cmd": "set", "index": 5, "r": 0, "g": 255, "b": 0 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"set"` |
| `index` | integer | yes | LED index, 0–(NUM_LEDS−1) |
| `r` | integer | yes | Red, 0–255 |
| `g` | integer | yes | Green, 0–255 |
| `b` | integer | yes | Blue, 0–255 |

**Error:** Returns `"index out of range"` if index is out of bounds.

---

#### `range` — Set a contiguous range of LEDs

```json
{ "cmd": "range", "from": 0, "to": 9, "r": 0, "g": 0, "b": 255 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"range"` |
| `from` | integer | yes | Start index (inclusive) |
| `to` | integer | yes | End index (inclusive) |
| `r` | integer | yes | Red, 0–255 |
| `g` | integer | yes | Green, 0–255 |
| `b` | integer | yes | Blue, 0–255 |

Both `from` and `to` are clamped to valid range automatically.

---

#### `multi` — Set multiple individual LEDs in one message

```json
{
  "cmd": "multi",
  "leds": [
    { "i": 0, "r": 255, "g": 0,   "b": 0 },
    { "i": 1, "r": 0,   "g": 255, "b": 0 },
    { "i": 2, "r": 0,   "g": 0,   "b": 255 }
  ]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"multi"` |
| `leds` | array | yes | Array of LED objects |
| `leds[].i` | integer | yes | LED index |
| `leds[].r` | integer | yes | Red, 0–255 |
| `leds[].g` | integer | yes | Green, 0–255 |
| `leds[].b` | integer | yes | Blue, 0–255 |

Out-of-range indices in the array are silently skipped.

---

#### `brightness` — Set global brightness

```json
{ "cmd": "brightness", "value": 128 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"brightness"` |
| `value` | integer | yes | Brightness, 0–255 (default 255) |

This is a device-level dimmer applied uniformly to all LEDs by FastLED. It is independent of the per-command `brightness` override available on `location` / `locations` commands. The per-command override scales the RGB values locally in the buffer before they are written — it does not touch this global setting.

---

#### `clear` — Turn all LEDs off

```json
{ "cmd": "clear" }
```

---

#### `effect` — Run a built-in animation

```json
{ "cmd": "effect", "name": "rainbow", "delay": 20 }
{ "cmd": "effect", "name": "chase", "r": 255, "g": 128, "b": 0, "delay": 60 }
{ "cmd": "effect", "name": "blink", "r": 0, "g": 0, "b": 255, "delay": 400 }
{ "cmd": "effect", "name": "test", "delay": 30 }
{ "cmd": "effect", "name": "stop" }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"effect"` |
| `name` | string | yes | `"rainbow"`, `"chase"`, `"blink"`, `"test"`, or `"stop"` |
| `r` | integer | no | Red (ignored for `rainbow`, `test`, and `stop`) |
| `g` | integer | no | Green (ignored for `rainbow`, `test`, and `stop`) |
| `b` | integer | no | Blue (ignored for `rainbow`, `test`, and `stop`) |
| `delay` | integer | no | Frame delay in ms, default 50 |

Effects run inside `renderTask` — the WebSocket stays responsive while an animation is playing. Sending `"stop"` clears all LEDs.

`"test"` lights each LED white one at a time (hardware walk test). Completes automatically and returns to idle.

---

#### `location` — Light a named location

Requires a mapping file to be uploaded to the device first (see HTTP API).

```json
{ "cmd": "location", "name": "A6", "r": 255, "g": 255, "b": 255 }
{ "cmd": "location", "name": "A6", "r": 255, "g": 0, "b": 0, "brightness": 180 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"location"` |
| `name` | string | yes | Location name, must exist in the mapping |
| `r` | integer | yes | Red, 0–255 |
| `g` | integer | yes | Green, 0–255 |
| `b` | integer | yes | Blue, 0–255 |
| `brightness` | integer | no | Per-command brightness override, 0–255. Scales the RGB values locally in the buffer. Defaults to 255 (full, unscaled). |

**Success response includes `leds`** — the number of LEDs that were lit.

**Error:** Returns `"location not found"` if the name is not in the mapping.

---

#### `locations` — Light multiple named locations in one message

```json
{
  "cmd": "locations",
  "brightness": 180,
  "items": [
    { "name": "A6", "r": 255, "g": 0,   "b": 0 },
    { "name": "B3", "r": 0,   "g": 255, "b": 0, "brightness": 100 },
    { "name": "C1", "r": 0,   "g": 0,   "b": 255 }
  ]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"locations"` |
| `brightness` | integer | no | Batch-level brightness default, 0–255 (default 255) |
| `items` | array | yes | Non-empty array of location objects, max `MAX_BATCH_ITEMS` (100) |
| `items[].name` | string | yes | Location name |
| `items[].r` | integer | yes | Red, 0–255 |
| `items[].g` | integer | yes | Green, 0–255 |
| `items[].b` | integer | yes | Blue, 0–255 |
| `items[].brightness` | integer | no | Per-item brightness override — takes precedence over the batch-level value |

All valid items are written to the buffer first, then a single `FastLED.show()` is called.

**Success response:**
```json
{ "status": "ok", "cmd": "locations", "applied_leds": 8, "missing_locations": 1, "invalid_items": 0 }
```

| Field | Description |
|-------|-------------|
| `applied_leds` | Total LED indices written to the strip |
| `missing_locations` | Items whose `name` was not found in the mapping |
| `invalid_items` | Items that were structurally invalid (e.g. missing `name`) |

---

#### `confirm` — Confirm a withdrawal

Lights the location green for 500 ms, then turns it off. Use this after a user takes an item to signal that the withdrawal was completed successfully.

```json
{ "cmd": "confirm", "name": "A6" }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"confirm"` |
| `name` | string | yes | Location name |

**Success response includes `leds`** — the number of LEDs that were flashed.

**Error:** Returns `"location not found"` if the name is not in the mapping.

> Blocks `renderTask` for ~500 ms while the flash is displayed. Any queued commands execute after the flash completes.

---

#### `location_clear` — Turn off a named location

```json
{ "cmd": "location_clear", "name": "A6" }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"location_clear"` |
| `name` | string | yes | Location name |

Sets all LEDs associated with that location to black (off). Returns `"location not found"` if the name does not exist.

---

### Quick Reference Table

| Command | What it does |
|---------|--------------|
| `fill` | All LEDs → one colour |
| `set` | Single LED by index |
| `range` | Contiguous range of LEDs |
| `multi` | Several individual LEDs in one shot |
| `brightness` | Global brightness multiplier |
| `clear` | All LEDs off |
| `effect` | Built-in animation (rainbow / chase / blink / stop) |
| `location` | Light a named location from the mapping |
| `locations` | Light multiple named locations in one message |
| `confirm` | Green flash on location for 500 ms, then off (withdrawal confirmed) |
| `location_clear` | Turn off a named location |
| `mode` | Signal warehouse operation mode (`idle`, `inventory`, `putaway`, `diag`) |
| `set_leds` | Change active LED count at runtime, saved to flash |
| `get_config` | Read current LED count and compile-time maximum |

---

#### `mode` — Signal the current warehouse operation mode

Tells the device which operation the middleware is currently running. Does **not** change any LEDs — use `location` / `locations` for that. The mode is reported in `GET /status` so the middleware can always read back the device state.

```json
{ "cmd": "mode", "name": "inventory" }
{ "cmd": "mode", "name": "idle" }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"mode"` |
| `name` | string | yes | `"idle"`, `"inventory"`, `"putaway"`, or `"diag"` |

> `"pick"` and `"batch"` are set automatically by `location` and `locations` commands and cannot be set via `mode`. `"effect"` and `"diag"` are set automatically by `effect` commands.

---

#### `set_leds` — Change the active LED count at runtime

Changes how many LEDs the device drives. Saves to flash immediately — survives reboot. Takes effect on the next frame without restart.

```json
{ "cmd": "set_leds", "value": 60 }
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `cmd` | string | yes | `"set_leds"` |
| `value` | integer | yes | New LED count, 1 – `MAX_LEDS` (compile-time ceiling, default 500) |

**Success response includes the confirmed count:**
```json
{ "status": "ok", "cmd": "set_leds", "num_leds": 60 }
```

> Any currently running effect is stopped and the strip is cleared before the count changes.

---

#### `get_config` — Read current LED configuration

```json
{ "cmd": "get_config" }
```

**Response:**
```json
{ "status": "ok", "cmd": "get_config", "num_leds": 60, "max_leds": 500 }
```

| Field | Description |
|-------|-------------|
| `num_leds` | Currently active LED count |
| `max_leds` | Compile-time hard ceiling (`MAX_LEDS` define) |

---

## HTTP REST API

The HTTP server on port 80 manages the location mapping file stored in the device's flash filesystem (LittleFS). The mapping survives reboots.

All responses are `application/json`. All endpoints include CORS headers (`Access-Control-Allow-Origin: *`).

### Endpoints

| Method | Path | Body | Description |
|--------|------|------|-------------|
| `GET` | `/status` | — | Device info and mapping status |
| `GET` | `/config` | — | Read active LED count and max |
| `POST` | `/config` | `{"num_leds": N}` | Set active LED count, saved to flash |
| `GET` | `/mapping` | — | Download the current mapping JSON |
| `PUT` | `/mapping` | JSON object | Upload a new mapping (replaces existing) |
| `POST` | `/upload` | JSON object | Legacy alias for `PUT /mapping` |
| `DELETE` | `/mapping` | — | Delete the mapping from flash |
| `GET` | `/diag` | — | Operational diagnostics (heap, uptime, WiFi, …) |
| `GET` | `/test` | — | Start hardware LED walk test |

---

#### `GET /status`

Returns device metadata.

```json
{
  "hostname":   "led-node-1",
  "shelf_id":   1,
  "ip":         "192.168.1.42",
  "num_leds":   60,
  "max_leds":   500,
  "locations":  12,
  "mapping_ok": true,
  "mode":       "idle"
}
```

`mode` is one of: `idle`, `pick`, `batch`, `inventory`, `putaway`, `effect`, `diag`.

---

#### `GET /diag`

Extended operational diagnostics for debugging and middleware integration.

```json
{
  "firmware":     "1.0.0",
  "hostname":     "led-node-1",
  "shelf_id":     1,
  "uptime_s":     3821,
  "heap_free":    218432,
  "wifi_status":  "connected",
  "wifi_rssi":    -62,
  "ip":           "192.168.1.42",
  "num_leds":     60,
  "max_leds":     500,
  "mapping_locs": 12,
  "mode":         "pick",
  "last_error":   null
}
```

`last_error` is `null` when no error has occurred since boot; otherwise a short string describing the most recent error (mapping parse failures, FS mount errors, etc.). It is **not** cleared automatically — use it for diagnostics, not for control flow.

---

#### `GET /test`

Starts the hardware LED walk test. Each LED lights white one at a time (30 ms per LED) so wiring can be verified. Completes automatically and returns the device to `idle` mode.

```json
{ "ok": true, "mode": "test_running" }
```

Equivalent to `{"cmd":"effect","name":"test"}` over WebSocket.

---

#### `GET /config`

Returns the current runtime configuration.

```json
{ "num_leds": 60, "max_leds": 500, "shelf_id": 2, "zone": "Regal A" }
```

`zone` is an empty string if not set.

---

#### `POST /config`

Update one or more runtime config fields. All fields are optional — only provided fields are changed. Changes are saved to flash immediately and survive reboot.

```
Content-Type: application/json
```

```json
{ "num_leds": 60 }
{ "shelf_id": 2, "zone": "Regal A" }
{ "num_leds": 60, "shelf_id": 2, "zone": "Regal A" }
```

| Field | Type | Description |
|-------|------|-------------|
| `num_leds` | integer | Active LED count, 1–`MAX_LEDS` |
| `shelf_id` | integer | Shelf number, 0–255 |
| `zone` | string | Zone label, max 31 chars (e.g. `"Regal A"`) |

**Success response:**
```json
{ "ok": true, "num_leds": 60, "shelf_id": 2, "zone": "Regal A" }
```

**Error responses:**
```json
{ "error": "invalid JSON" }                   // 400
{ "error": "no recognised fields" }           // 400 — body has no known keys
{ "error": "num_leds must be 1–500" }         // 400
{ "error": "shelf_id must be 0–255" }         // 400
{ "error": "zone too long, max 31" }          // 400
{ "error": "render queue full" }              // 503 — transient, retry
```

---

#### `GET /mapping`

Returns the raw mapping JSON as stored on the device.

```json
{ "A6": 5, "B3": 12, "C1": [3, 4, 5] }
```

Returns `404` if no mapping has been uploaded yet.

---

#### `POST /upload`

Upload a new mapping. The body must be a valid JSON object where each key is a location name and each value is either a single LED index (integer) or an array of LED indices.

```
Content-Type: application/json
```

```json
{
  "A6": 5,
  "B3": 12,
  "C1": [3, 4, 5],
  "D2": 0
}
```

**Validation rules (enforced before writing to flash):**
- Root must be a JSON object
- Max `MAX_MAPPING_LOCATIONS` (200) entries
- Each key: max `MAX_LOCATION_NAME_LEN` (32) characters
- Each value: integer or non-empty integer array
- All indices: must be in range `0 – MAX_LEDS−1`

**Success response:**
```json
{ "ok": true, "locations": 4 }
```

**Error responses:**
```json
{ "error": "invalid JSON: ..." }              // 400 — body is not valid JSON
{ "error": "empty body" }                     // 400 — no body sent
{ "error": "mapping must be a JSON object" }  // 400 — root is not an object
{ "error": "too many locations, max 200" }    // 400 — entry count exceeds limit
{ "error": "location name too long: 'X'" }   // 400 — key exceeds 32 chars
{ "error": "index out of range for 'X'" }    // 400 — single-int index out of bounds
{ "error": "empty array for 'X'" }           // 400 — value is an empty array
{ "error": "invalid index in array for 'X'"} // 400 — array contains non-int or out-of-range index
{ "error": "invalid value for 'X': ..." }    // 400 — value is neither int nor int[]
{ "error": "fs write failed" }               // 500 — filesystem error
```

The new mapping is loaded into RAM immediately — no reboot required.
Uses atomic write (temp file → rename) to protect against power loss mid-upload.

---

#### `DELETE /mapping`

Deletes the mapping file from flash and clears it from RAM.

```json
{ "ok": true }
```

---

### Mapping File Format

```json
{
  "A6":   5,
  "B3":   12,
  "C1":   [3, 4, 5],
  "LOGO": [0, 1, 2, 10, 11, 12]
}
```

- Keys are arbitrary strings — use whatever naming convention fits your project
- A value of `5` means location maps to LED index 5
- A value of `[3, 4, 5]` means the location maps to LED indices 3, 4, and 5 — all are lit simultaneously
- Maximum mapping size is ~300 locations (8 KB RAM budget)
- The file is stored at `/mapping.json` in LittleFS

---

## Code Examples

### JavaScript (browser / Node.js)

```javascript
const ws = new WebSocket("ws://led-node-1.local:81");

ws.onopen = () => {
  // Fill all red
  ws.send(JSON.stringify({ cmd: "fill", r: 255, g: 0, b: 0 }));
};

ws.onmessage = (e) => {
  const reply = JSON.parse(e.data);
  console.log(reply); // { status: "ok", cmd: "fill" }
};

// Light a location
ws.send(JSON.stringify({ cmd: "location", name: "A6", r: 0, g: 255, b: 0 }));

// Batch locations
ws.send(JSON.stringify({
  cmd: "locations",
  items: [
    { name: "A6", r: 255, g: 0,   b: 0 },
    { name: "B3", r: 0,   g: 255, b: 0, brightness: 120 },
  ]
}));
```

### Python

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://led-node-1.local:81") as ws:

        # Fill blue
        await ws.send(json.dumps({"cmd": "fill", "r": 0, "g": 0, "b": 255}))
        print(await ws.recv())

        # Light location
        await ws.send(json.dumps({"cmd": "location", "name": "A6", "r": 255, "g": 255, "b": 0}))
        print(await ws.recv())

        # Clear
        await ws.send(json.dumps({"cmd": "clear"}))
        print(await ws.recv())

asyncio.run(main())
```

### Upload a mapping (Python)

```python
import requests, json

mapping = {
    "A6": 5,
    "B3": 12,
    "C1": [3, 4, 5],
}

r = requests.post(
    "http://led-node-1.local:80/upload",
    json=mapping,
    headers={"Content-Type": "application/json"}
)
print(r.json())  # {"ok": True, "locations": 3}
```

### wscat (command line testing)

```bash
npm install -g wscat
wscat -c ws://led-node-1.local:81

# Then type commands:
> {"cmd":"fill","r":255,"g":0,"b":0}
< {"status":"ok","cmd":"fill"}

> {"cmd":"effect","name":"rainbow","delay":20}
< {"status":"ok","cmd":"effect"}

> {"cmd":"location","name":"A6","r":0,"g":255,"b":0}
< {"status":"ok","cmd":"location","leds":1}

> {"cmd":"set_leds","value":60}
< {"status":"ok","cmd":"set_leds","num_leds":60}

> {"cmd":"get_config"}
< {"status":"ok","cmd":"get_config","num_leds":60,"max_leds":500}
```

### Set LED count via HTTP (Python)

```python
import requests

# Read current config
r = requests.get("http://led-node-1.local:80/config")
print(r.json())  # {"num_leds": 30, "max_leds": 500}

# Change to 60 LEDs
r = requests.post(
    "http://led-node-1.local:80/config",
    json={"num_leds": 60},
    headers={"Content-Type": "application/json"}
)
print(r.json())  # {"ok": True, "num_leds": 60}
```

---

## Error Reference

| Message | Cause |
|---------|-------|
| `"JSON parse error"` | Message body is not valid JSON |
| `"missing 'cmd'"` | The `cmd` field is absent |
| `"unknown command"` | `cmd` value is not recognised |
| `"index out of range"` | `set` command index outside 0–(num_leds−1) |
| `"missing 'name'"` | `effect`, `location`, or `location_clear` missing `name` field |
| `"unknown effect"` | `effect.name` is not `rainbow`, `chase`, `blink`, or `stop` |
| `"location not found"` | Location name not in the current mapping |
| `"missing 'items'"` | `locations` command missing `items` array |
| `"value must be 1–500"` | `set_leds` value out of range (upper bound = `MAX_LEDS`) |

---

## Device Configuration

### Runtime (changeable without reflashing)

| Setting | How to change | Persists reboot? |
|---------|--------------|-----------------|
| Active LED count | `set_leds` WS command or `POST /config` | Yes — stored in `/config.json` |
| Shelf ID | `POST /config` | Yes — stored in `/config.json` |
| Zone name | `POST /config` | Yes — stored in `/config.json` |
| Location mapping | `PUT /mapping` or `POST /upload` | Yes — stored in `/mapping.json` |
| Brightness | `brightness` WS command | No — resets to `MAX_BRIGHTNESS` on reboot |

### Compile-time (requires reflashing to change)

| Constant | Default | Description |
|----------|---------|-------------|
| `HOSTNAME` | `led-node-1` | mDNS hostname — change to `led-node-2`, `led-node-3` (compile-time only) |
| `LED_PIN` | `8` | GPIO data pin |
| `MAX_LEDS` | `500` | Hard ceiling for LED count; allocates RAM (`3 × MAX_LEDS` bytes) |
| `LED_TYPE` | `WS2812B` | LED chipset |
| `COLOR_ORDER` | `GRB` | Colour byte order |
| `MAX_BRIGHTNESS` | `255` | Power-on brightness |
