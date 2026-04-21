#pragma once

// ─────────────────────────────────────────────
//  USER CONFIGURATION  (edit these per device)
// ─────────────────────────────────────────────
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"
#define HOSTNAME          "led-node-1"  // compile-time only (mDNS hostname)
#define SHELF_ID_DEFAULT  1            // default shelf number — overridable at runtime via POST /config
#define FIRMWARE_VERSION  "1.0.0"      // bump on every flash to production hardware

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

// Device identity — runtime-configurable via POST /config, stored in config.json
#define MAX_ZONE_LEN  31   // max characters for the zone name

// Mapping constraints — enforced at upload time and at runtime
#define MAX_LOCATION_NAME_LEN  32    // max characters per location key
#define MAX_MAPPING_LOCATIONS  200   // max entries per mapping upload
#define MAX_LEDS_PER_LOCATION  16    // max LED indices a single location may map to
#define MAX_BATCH_ITEMS        100   // max items in a locations batch command
// ─────────────────────────────────────────────
