# ILLAR ESP32 LED Controller

ESP32-C6 firmware for WS2812B LED shelf guidance, part of the ILLAR warehouse system.

## Setup

1. Open `esp32_led_ws.ino` in the Arduino IDE.
2. At the top of the file, set your WiFi credentials and a unique hostname per device:
   ```cpp
   const char* WIFI_SSID     = "YOUR_SSID";
   const char* WIFI_PASSWORD = "YOUR_PASSWORD";
   const char* HOSTNAME      = "led-node-1";   // change per shelf (led-node-1, led-node-2, …)
   ```
3. Select board **ESP32C6 Dev Module** and flash.

That's it. Everything else — LED count, location mapping, and real-time control — is done at runtime via the API.

## Wiring

| Signal | ESP32-C6 Pin |
|---|---|
| WS2812B Data | **GPIO 8** |
| 5V | 5V (from PSU) |
| GND | GND |

## API

See **[API.md](API.md)** for the full reference.

Each flashed device exposes:
- **HTTP REST** on port `80` — configure LED count, upload location mapping
- **WebSocket** on port `81` — real-time LED control

Devices are reachable by mDNS hostname (e.g. `led-node-1.local`) or IP address.

## Required Libraries

Install via Arduino Library Manager:
- `FastLED` by Daniel Garcia
- `WebSockets` by Markus Sattler
- `ArduinoJson` by Benoit Blanchon (v6)
