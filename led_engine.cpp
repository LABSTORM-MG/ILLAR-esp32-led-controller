#include <Arduino.h>
#include <FastLED.h>
#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"

// ── Global definitions ────────────────────────
CRGB              leds[MAX_LEDS];
SemaphoreHandle_t ledMutex      = NULL;
int               numLeds       = 30;     // overwritten from flash in loadConfig()
volatile bool     wifiConnected = false;
bool              statusChanged = false;
Effect            activeEffect  = EFF_NONE;
uint8_t           effR=255, effG=255, effB=255;
uint16_t          effDelay      = 50;
DeviceMode        currentMode   = MODE_IDLE;
TaskHandle_t      effectTaskHandle = NULL;

// ── Mutex wrappers ────────────────────────────
void ledAcquire() { xSemaphoreTake(ledMutex, portMAX_DELAY); }
void ledRelease() { xSemaphoreGive(ledMutex); }

// ── LED helpers ───────────────────────────────
int lightLocation(const char* name, uint8_t r, uint8_t g, uint8_t b) {
  if (!locationMap.containsKey(name)) return -1;
  JsonVariant entry = locationMap[name];
  int count = 0;
  if (entry.is<JsonArray>()) {
    for (JsonVariant v : entry.as<JsonArray>()) {
      int idx = v.as<int>();
      if (idx >= 0 && idx < numLeds) { leds[idx] = CRGB(r,g,b); count++; }
    }
  } else {
    int idx = entry.as<int>();
    if (idx >= 0 && idx < numLeds) { leds[idx] = CRGB(r,g,b); count = 1; }
  }
  return count;
}

void setAll(uint8_t r, uint8_t g, uint8_t b) {
  fill_solid(leds, numLeds, CRGB(r,g,b)); FastLED.show();
}

void clearAll() {
  fill_solid(leds, MAX_LEDS, CRGB::Black); FastLED.show();
}

void showSystemStatus() {
  ledAcquire();
  leds[0] = leds[1] = leds[2] = CRGB::Black;
  if (!wifiConnected) {
    leds[0] = leds[1] = leds[2] = CRGB::Red;
  } else if (locationMap.size() == 0) {
    leds[0] = leds[1] = leds[2] = CRGB(255, 80, 0); // orange
  }
  FastLED.show();
  ledRelease();
}

// ── Effects task ──────────────────────────────
void effectTask(void* pv) {
  uint8_t hue=0; uint16_t pos=0; bool blinkOn=false;
  while (true) {
    ledAcquire();
    Effect   eff      = activeEffect;  // local copy while locked
    uint16_t delay_ms = effDelay;      // local copy — used after release
    switch (eff) {
      case EFF_RAINBOW:
        fill_rainbow(leds, numLeds, hue++, 7); FastLED.show();
        ledRelease(); vTaskDelay(pdMS_TO_TICKS(delay_ms)); break;
      case EFF_CHASE:
        fill_solid(leds, numLeds, CRGB::Black);
        leds[pos % numLeds] = CRGB(effR,effG,effB); FastLED.show(); pos++;
        ledRelease(); vTaskDelay(pdMS_TO_TICKS(delay_ms)); break;
      case EFF_BLINK:
        blinkOn = !blinkOn;
        if (blinkOn) fill_solid(leds, numLeds, CRGB(effR,effG,effB));
        else         fill_solid(leds, numLeds, CRGB::Black);
        FastLED.show();
        ledRelease(); vTaskDelay(pdMS_TO_TICKS(delay_ms)); break;
      default:
        ledRelease(); vTaskDelay(pdMS_TO_TICKS(50)); break;
    }
  }
}

// ── Hardware test walk task ───────────────────
// Lights each LED white one at a time. Useful for verifying wiring during installation.
// Deletes itself when done.
void testWalkTask(void* pv) {
  for (int i = 0; i < numLeds; i++) {
    ledAcquire();
    leds[i] = CRGB::White;
    FastLED.show();
    ledRelease();
    vTaskDelay(pdMS_TO_TICKS(30));
    ledAcquire();
    leds[i] = CRGB::Black;
    FastLED.show();
    ledRelease();
  }
  currentMode = MODE_IDLE;
  vTaskDelete(NULL);
}
