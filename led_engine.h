#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"

// ── Device operational mode ───────────────────
// Updated by WS/HTTP handlers. Informational — reported in /status for middleware.
enum DeviceMode {
  MODE_IDLE,       // LEDs off, no active command
  MODE_PICK,       // Single location lit for withdrawal
  MODE_BATCH,      // Batch locations lit
  MODE_INVENTORY,  // Inventory check mode
  MODE_PUTAWAY,    // Store/return mode
  MODE_EFFECT,     // Aesthetic effect running
  MODE_DIAG        // Hardware test running
};

// ── Effect types ──────────────────────────────
enum Effect { EFF_NONE, EFF_RAINBOW, EFF_CHASE, EFF_BLINK };

// ── Globals — defined in led_engine.cpp ───────
extern CRGB             leds[];
extern SemaphoreHandle_t ledMutex;
extern int              numLeds;
extern volatile bool    wifiConnected;
extern bool             statusChanged;
extern Effect           activeEffect;
extern uint8_t          effR, effG, effB;
extern uint16_t         effDelay;
extern DeviceMode       currentMode;
extern TaskHandle_t     effectTaskHandle;

// ── Mutex wrappers ────────────────────────────
// All writes to leds[] and FastLED.show() must be bracketed with these.
// lightLocation(), setAll(), clearAll() do NOT acquire internally — callers hold the lock.
void ledAcquire();
void ledRelease();

// ── LED helpers ───────────────────────────────
// Writes location LEDs to buffer only. Caller must hold ledMutex and call FastLED.show().
// Returns number of LEDs written, or -1 if location not found.
int  lightLocation(const char* name, uint8_t r, uint8_t g, uint8_t b);

// Fill active range and show. Caller must hold ledMutex.
void setAll(uint8_t r, uint8_t g, uint8_t b);

// Clear full MAX_LEDS buffer and show. Caller must hold ledMutex.
void clearAll();

// ── Status indicator ──────────────────────────
// First 3 LEDs: red = no WiFi, orange = no mapping, off = all good.
// Call only from main loop (not from effectTask).
void showSystemStatus();

// ── FreeRTOS tasks ────────────────────────────
void effectTask(void* pv);
void testWalkTask(void* pv);
