#include <Arduino.h>
#include <FastLED.h>
#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"

// ── Global definitions ────────────────────────
CRGB              leds[MAX_LEDS];
QueueHandle_t     renderQueue          = NULL;
char              lastError[64]        = "";
int               shelfId              = SHELF_ID_DEFAULT;
char              zone[MAX_ZONE_LEN+1] = "";
BatchLedSet       batchBuffer[BATCH_BUFFER_SIZE];
SemaphoreHandle_t batchMutex    = NULL;
int               numLeds       = MAX_LEDS; // overwritten from flash in loadConfig()
volatile bool     wifiConnected = false;
bool              statusChanged = false;
DeviceMode        currentMode   = MODE_IDLE;

// ── Queue helper ──────────────────────────────
void setLastError(const char* msg) {
  strncpy(lastError, msg, sizeof(lastError) - 1);
  lastError[sizeof(lastError) - 1] = '\0';
}

bool ledEnqueue(const LedCommand& cmd) {
  return xQueueSend(renderQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE;
}

void showSystemStatus() {
  LedCommand cmd; cmd.type = CMD_STATUS_UPDATE;
  ledEnqueue(cmd);
}

// ── Render task ───────────────────────────────
// Sole owner of leds[] and FastLED.show().
// Processes queued commands immediately; runs effect frames on queue timeout.
void renderTask(void* pv) {
  Effect   activeEffect = EFF_NONE;
  uint8_t  effR = 255, effG = 255, effB = 255;
  uint16_t effDelay = 50;
  uint8_t  hue = 0;
  uint16_t effectPos = 0;
  bool     blinkOn = false;
  uint8_t  pulseMin = 30, pulseMax = 255, pulseSpeed = 3, pulsePhase = 0;

  while (true) {
    TickType_t wait = (activeEffect != EFF_NONE)
      ? pdMS_TO_TICKS(effDelay)
      : portMAX_DELAY;

    LedCommand cmd;
    if (xQueueReceive(renderQueue, &cmd, wait) == pdTRUE) {
      switch (cmd.type) {

        case CMD_FILL:
          fill_solid(leds, numLeds, CRGB(cmd.fill.r, cmd.fill.g, cmd.fill.b));
          activeEffect = EFF_NONE; currentMode = MODE_IDLE;
          FastLED.show();
          break;

        case CMD_SET:
          if (cmd.set.idx < (uint16_t)numLeds) {
            leds[cmd.set.idx] = CRGB(cmd.set.r, cmd.set.g, cmd.set.b);
            activeEffect = EFF_NONE; currentMode = MODE_IDLE;
            FastLED.show();
          }
          break;

        case CMD_RANGE: {
          int from = constrain((int)cmd.range.from, 0, numLeds-1);
          int to   = constrain((int)cmd.range.to,   0, numLeds-1);
          CRGB col(cmd.range.r, cmd.range.g, cmd.range.b);
          for (int i = from; i <= to; i++) leds[i] = col;
          activeEffect = EFF_NONE; currentMode = MODE_IDLE;
          FastLED.show();
          break;
        }

        case CMD_BRIGHTNESS:
          FastLED.setBrightness(cmd.brightness.value);
          FastLED.show();
          break;

        case CMD_CLEAR:
          fill_solid(leds, MAX_LEDS, CRGB::Black);
          activeEffect = EFF_NONE; currentMode = MODE_IDLE;
          FastLED.show();
          break;

        case CMD_EFFECT_START:
          activeEffect = cmd.effect.eff;
          effR = cmd.effect.r; effG = cmd.effect.g; effB = cmd.effect.b;
          effDelay = cmd.effect.delay_ms ? cmd.effect.delay_ms : 50;
          pulseMin   = cmd.effect.pulseMin   ? cmd.effect.pulseMin   : 30;
          pulseMax   = cmd.effect.pulseMax   ? cmd.effect.pulseMax   : 255;
          pulseSpeed = cmd.effect.pulseSpeed ? cmd.effect.pulseSpeed : 3;
          hue = 0; effectPos = 0; blinkOn = false; pulsePhase = 0;
          currentMode = (activeEffect == EFF_TEST_WALK) ? MODE_DIAG : MODE_EFFECT;
          break;

        case CMD_EFFECT_STOP:
          activeEffect = EFF_NONE; currentMode = MODE_IDLE;
          fill_solid(leds, MAX_LEDS, CRGB::Black);
          FastLED.show();
          break;

        case CMD_LOCATION: {
          const Location* loc = findLocation(cmd.location.name);
          if (loc) {
            for (int i = 0; i < loc->count; i++) {
              uint16_t idx = loc->indices[i];
              if (idx < (uint16_t)numLeds)
                leds[idx] = CRGB(cmd.location.r, cmd.location.g, cmd.location.b);
            }
            activeEffect = EFF_NONE; currentMode = MODE_PICK;
            FastLED.show();
          }
          break;
        }

        case CMD_LOCATION_CLEAR: {
          const Location* loc = findLocation(cmd.locationClear.name);
          if (loc) {
            for (int i = 0; i < loc->count; i++) {
              uint16_t idx = loc->indices[i];
              if (idx < (uint16_t)numLeds) leds[idx] = CRGB::Black;
            }
            currentMode = MODE_IDLE;
            FastLED.show();
          }
          break;
        }

        case CMD_CONFIRM: {
          const Location* loc = findLocation(cmd.confirm.name);
          if (loc) {
            for (int i = 0; i < loc->count; i++) {
              uint16_t idx = loc->indices[i];
              if (idx < (uint16_t)numLeds) leds[idx] = CRGB::Green;
            }
            FastLED.show();
            vTaskDelay(pdMS_TO_TICKS(500));
            for (int i = 0; i < loc->count; i++) {
              uint16_t idx = loc->indices[i];
              if (idx < (uint16_t)numLeds) leds[idx] = CRGB::Black;
            }
            currentMode = MODE_IDLE;
            FastLED.show();
          }
          break;
        }

        case CMD_BATCH_APPLY:
          for (int i = 0; i < cmd.batch.count; i++) {
            uint16_t idx = batchBuffer[i].idx;
            if (idx < (uint16_t)numLeds) leds[idx] = batchBuffer[i].color;
          }
          activeEffect = EFF_NONE; currentMode = cmd.batch.mode;
          FastLED.show();
          xSemaphoreGive(batchMutex);  // release staging buffer for next caller
          break;

        case CMD_SET_LEDS:
          fill_solid(leds, MAX_LEDS, CRGB::Black);
          activeEffect = EFF_NONE; currentMode = MODE_IDLE;
          numLeds = cmd.setLeds.value;
          FastLED.show();
          saveConfig();
          break;

        case CMD_MODE:
          currentMode = cmd.setMode.mode;
          break;

        case CMD_STATUS_UPDATE:
          leds[0] = leds[1] = leds[2] = CRGB::Black;
          if (!wifiConnected) {
            leds[0] = leds[1] = leds[2] = CRGB::Red;
          } else if (locationCount == 0) {
            leds[0] = leds[1] = leds[2] = CRGB(255, 80, 0);
          }
          FastLED.show();
          break;
      }

    } else {
      // Timeout — advance one effect frame
      switch (activeEffect) {
        case EFF_RAINBOW:
          // Paint the same rainbow onto each tier's block individually —
          // a single fill_rainbow() across the flat buffer would instead
          // spread one continuous rainbow across all tiers, so each tier
          // shows a different slice of the color wheel instead of matching.
          for (int t = 0; t < NUM_TIERS; t++) {
            int tierStart  = t * MAX_LEDS_PER_TIER;
            int tierActive = numLeds - tierStart;
            if (tierActive <= 0) break;
            if (tierActive > MAX_LEDS_PER_TIER) tierActive = MAX_LEDS_PER_TIER;
            fill_rainbow(leds + tierStart, tierActive, hue, 7);
          }
          hue++;
          FastLED.show();
          break;
        case EFF_CHASE:
          fill_solid(leds, numLeds, CRGB::Black);
          leds[effectPos % numLeds] = CRGB(effR, effG, effB);
          effectPos++;
          FastLED.show();
          break;
        case EFF_BLINK:
          blinkOn = !blinkOn;
          fill_solid(leds, numLeds, blinkOn ? CRGB(effR, effG, effB) : CRGB::Black);
          FastLED.show();
          break;
        case EFF_PULSE: {
          uint8_t bright = pulseMin + scale8(sin8(pulsePhase), pulseMax - pulseMin);
          fill_solid(leds, numLeds,
            CRGB(scale8(effR, bright), scale8(effG, bright), scale8(effB, bright)));
          FastLED.show();
          pulsePhase += pulseSpeed;
          break;
        }
        case EFF_TEST_WALK:
          if (effectPos > 0) leds[(effectPos-1) % numLeds] = CRGB::Black;
          leds[effectPos % numLeds] = CRGB::White;
          FastLED.show();
          effectPos++;
          if ((int)effectPos > numLeds) {
            fill_solid(leds, MAX_LEDS, CRGB::Black);
            FastLED.show();
            activeEffect = EFF_NONE; currentMode = MODE_IDLE;
          }
          break;
        default:
          break;
      }
    }
  }
}
