#pragma once

// ─────────────────────────────────────────────
//  USER CONFIGURATION  (edit these per device)
// ─────────────────────────────────────────────
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"
#define HOSTNAME      "led-node-1"   // change per device (led-node-1, led-node-2, …)

#define LED_PIN        8
#define LED_TYPE       WS2812B
#define COLOR_ORDER    GRB
#define MAX_BRIGHTNESS 255

// Maximum number of LEDs this device will ever drive.
// The actual active count is loaded from flash at boot (set via HTTP or WS).
// Increase if your strip is larger — uses 3 bytes of RAM per LED.
#define MAX_LEDS  500

#define CONFIG_FILE   "/config.json"
#define MAPPING_FILE  "/mapping.json"
// ─────────────────────────────────────────────
