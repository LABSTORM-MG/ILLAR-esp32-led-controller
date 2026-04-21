#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "config.h"

// ── Device operational mode ───────────────────
enum DeviceMode {
  MODE_IDLE,
  MODE_PICK,
  MODE_BATCH,
  MODE_INVENTORY,
  MODE_PUTAWAY,
  MODE_EFFECT,
  MODE_DIAG
};

// ── Effect types ──────────────────────────────
enum Effect : uint8_t { EFF_NONE, EFF_RAINBOW, EFF_CHASE, EFF_BLINK, EFF_TEST_WALK };

// ── LED command types ─────────────────────────
enum CmdType : uint8_t {
  CMD_FILL,
  CMD_SET,
  CMD_RANGE,
  CMD_BRIGHTNESS,
  CMD_CLEAR,
  CMD_EFFECT_START,
  CMD_EFFECT_STOP,
  CMD_LOCATION,
  CMD_LOCATION_CLEAR,
  CMD_CONFIRM,        // green flash on location → off (withdrawal confirmed)
  CMD_BATCH_APPLY,    // applies batchBuffer[0..batch.count-1] to the strip
  CMD_SET_LEDS,
  CMD_MODE,           // explicit warehouse mode transition, no LED change
  CMD_STATUS_UPDATE,
};

// ── Batch staging buffer ──────────────────────
// Used by multi and locations commands.
// Protocol: caller acquires batchMutex, fills batchBuffer, enqueues CMD_BATCH_APPLY.
// renderTask releases batchMutex after applying the buffer.
#define BATCH_BUFFER_SIZE (MAX_BATCH_ITEMS * MAX_LEDS_PER_LOCATION)
struct BatchLedSet { uint16_t idx; CRGB color; };

// ── LED command (queue item) ──────────────────
struct LedCommand {
  CmdType type;
  union {
    struct { uint8_t r, g, b; }                                     fill;
    struct { uint16_t idx; uint8_t r, g, b; }                       set;
    struct { uint16_t from, to; uint8_t r, g, b; }                  range;
    struct { uint8_t value; }                                        brightness;
    struct { Effect eff; uint8_t r, g, b; uint16_t delay_ms; }      effect;
    struct { char name[MAX_LOCATION_NAME_LEN+1]; uint8_t r, g, b; } location;
    struct { char name[MAX_LOCATION_NAME_LEN+1]; }                  locationClear;
    struct { char name[MAX_LOCATION_NAME_LEN+1]; }                  confirm;
    struct { int count; DeviceMode mode; }                           batch;
    struct { int value; }                                            setLeds;
    struct { DeviceMode mode; }                                      setMode;
  };
};

// ── Globals — defined in led_engine.cpp ───────
extern CRGB              leds[];
extern QueueHandle_t     renderQueue;
extern char              lastError[];  // last system error, empty string if none
extern int               shelfId;      // runtime-configurable, persisted in config.json
extern char              zone[];       // optional zone label, e.g. "Regal A"
extern BatchLedSet       batchBuffer[];
extern SemaphoreHandle_t batchMutex;
extern int               numLeds;
extern volatile bool     wifiConnected;  // written from WiFi event callbacks
extern bool              statusChanged;
extern DeviceMode        currentMode;

// ── Error reporting ───────────────────────────
// Records the last system error for /diag. Pass "" to clear.
void setLastError(const char* msg);

// ── Queue helper ──────────────────────────────
// Enqueues a command (blocks up to 100 ms). Returns false if queue is full.
bool ledEnqueue(const LedCommand& cmd);

// ── Status indicator ──────────────────────────
// Enqueues CMD_STATUS_UPDATE. First 3 LEDs show WiFi/mapping state.
void showSystemStatus();

// ── Render task ───────────────────────────────
// The only task that writes to leds[] and calls FastLED.show().
// Also handles all effects internally (replaces the old effectTask).
void renderTask(void* pv);
