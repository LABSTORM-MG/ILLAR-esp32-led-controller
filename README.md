# ILLAR ESP32 LED Controller

ESP32-C6 firmware for WS2812B LED shelf guidance, part of the ILLAR warehouse system.

Used as the `led-controller` git submodule of the main [storage-room](https://github.com/dejhfm/storage-room) repo, which also covers the frontend, InvenTree backend, and system-wide architecture.

## Setup

1. Open `esp32_led_ws.ino` in the Arduino IDE (the sketch folder must be named `esp32_led_ws`).
2. Edit `config.h` and set your WiFi credentials, a unique hostname per device, and (if needed) the tier pins:
   ```cpp
   #define WIFI_SSID     "YOUR_SSID"
   #define WIFI_PASSWORD "YOUR_PASSWORD"
   #define HOSTNAME      "led-node-1"   // change per shelf (led-node-1, led-node-2, …)
   ```
3. Select board **ESP32C6 Dev Module**, partition scheme **"Default 4MB with spiffs"** (or any scheme with a filesystem — needed for LittleFS, which stores the persisted config/mapping), and flash.

That's it. Everything else — LED count, location mapping, and real-time control — is done at runtime via the API.

## Wiring

Each shelf tier ("Etage") gets its own data pin and its own 5V injection point — LEDs are **not** daisy-chained across tiers, which avoids voltage-drop dimming/yellowing at the far end of a long run.

| Signal | ESP32-C6 Pin | Config macro |
|---|---|---|
| Tier 1 data | GPIO 8  | `LED_PIN_TIER_1` |
| Tier 2 data | GPIO 9  | `LED_PIN_TIER_2` |
| Tier 3 data | GPIO 10 | `LED_PIN_TIER_3` |
| Tier 4 data | GPIO 11 | `LED_PIN_TIER_4` |
| Tier 5 data | GPIO 18 | `LED_PIN_TIER_5` |
| 5V (per tier) | 5V from PSU, fed separately to each tier | — |
| GND | GND | — |

Pin numbers are defined in `config.h` — verify them against your actual wiring/board before flashing, and avoid strapping pins where possible. Note that GPIO 12/13 are the ESP32-C6's native USB D-/D+ lines and are unusable as LED outputs (FastLED refuses to compile against them). Max LEDs per tier is `MAX_LEDS_PER_TIER` (default 100; total addressable buffer is `NUM_TIERS × MAX_LEDS_PER_TIER`).

## Dev Tools

Two standalone HTML pages (no build step, open directly in a browser) for testing and setup without writing WebSocket/HTTP calls by hand:
- **`dev_testing_dashboard.html`** — connect to one or more nodes, fire LED commands (fill/set/range/effects), set active LED count, upload/inspect the location mapping.
- **`mapping_tool.html`** — visual editor for building a shelf's `mapping.json` (location name → LED index/indices).

## API

See **[API.md](API.md)** for the full reference.

Each flashed device exposes:
- **HTTP REST** on port `80` — configure LED count, upload location mapping
- **WebSocket** on port `81` — real-time LED control

Devices are reachable by mDNS hostname (e.g. `led-node-1.local`) or IP address.

## Required Libraries

Install via Arduino Library Manager (also install **esp32 by Espressif Systems** via Boards Manager):
- `FastLED` by Daniel Garcia
- `WebSockets` by Markus Sattler
- `ArduinoJson` by Benoit Blanchon (v6)
