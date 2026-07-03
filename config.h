#pragma once

// ─────────────────────────────────────────────
//  USER CONFIGURATION  (edit these per device)
// ─────────────────────────────────────────────
#define WIFI_SSID     "Lennart"
#define WIFI_PASSWORD "Lennart1234#"
#define HOSTNAME          "led-node-1"  // compile-time only (mDNS hostname)
#define SHELF_ID_DEFAULT  1            // default shelf number — overridable at runtime via POST /config
#define FIRMWARE_VERSION  "1.0.0"      // bump on every flash to production hardware

// One data pin per shelf tier ("Etage"). Each tier is wired as its own short
// run fed with its own 5V injection point, instead of one long daisy-chained
// strip — avoids voltage-drop dimming/yellowing at the far end of the shelf.
// Verify pin numbers against your ESP32-C6 Dev Module before flashing —
// avoid strapping pins where your wiring allows. GPIO12/13 are the C6's
// native USB D-/D+ lines and cannot be used as LED outputs.
#define NUM_TIERS       5
#define LED_PIN_TIER_1  8
#define LED_PIN_TIER_2  9
#define LED_PIN_TIER_3  10
#define LED_PIN_TIER_4  11
#define LED_PIN_TIER_5  18

#define LED_TYPE       WS2812B
#define COLOR_ORDER    GRB
#define MAX_BRIGHTNESS 255

// Maximum number of LEDs per physical tier strip — uses 3 bytes of RAM per LED.
// Total addressable buffer is NUM_TIERS * MAX_LEDS_PER_TIER; the active count
// within that (numLeds) is loaded from flash at boot (set via HTTP or WS).
#define MAX_LEDS_PER_TIER  100
#define MAX_LEDS           (NUM_TIERS * MAX_LEDS_PER_TIER)

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
