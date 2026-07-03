/*
 * ILLAR ESP32-C6 LED Node
 * ========================
 * Edit config.h to set WiFi credentials, hostname, and LED tier pins before flashing.
 *
 * Libraries required (Arduino Library Manager):
 *   - FastLED           (by Daniel Garcia)
 *   - WebSockets        (by Markus Sattler)
 *   - ArduinoJson       (by Benoit Blanchon, v6)
 *   - ESPmDNS           (bundled with ESP32 core)
 *   Also install: esp32 by Espressif Systems (via Boards Manager)
 *
 * Filesystem: LittleFS (bundled with ESP32 core >= 2.0.0)
 * Board: "ESP32C6 Dev Module"
 *        Partition scheme: "Default 4MB with spiffs" or any with FS
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <FastLED.h>
#include <ArduinoJson.h>

#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"
#include "http_api.h"
#include "ws_api.h"

// Server objects — declared here, accessed via extern in http_api.cpp and ws_api.cpp
WebSocketsServer webSocket(81);
WebServer        httpServer(80);

// ── Setup ─────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  // Queue and mutex must exist before renderTask starts
  renderQueue = xQueueCreate(8, sizeof(LedCommand));
  batchMutex  = xSemaphoreCreateMutex();

  // Mount filesystem and load persisted config + mapping
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Mount failed! Using compile-time defaults.");
    setLastError("LittleFS mount failed");
  } else {
    Serial.println("[FS] Mounted.");
    loadConfig();
    loadMapping();
  }

  // FastLED — one controller per shelf tier, each on its own pin, each
  // covering a fixed MAX_LEDS_PER_TIER slice of the shared leds[] buffer.
  // Active length across the whole buffer is controlled via numLeds.
  // Direct leds[] access is safe here — renderTask hasn't started yet.
  FastLED.addLeds<LED_TYPE, LED_PIN_TIER_1, COLOR_ORDER>(leds + 0*MAX_LEDS_PER_TIER, MAX_LEDS_PER_TIER).setCorrection(TypicalLEDStrip);
  FastLED.addLeds<LED_TYPE, LED_PIN_TIER_2, COLOR_ORDER>(leds + 1*MAX_LEDS_PER_TIER, MAX_LEDS_PER_TIER).setCorrection(TypicalLEDStrip);
  FastLED.addLeds<LED_TYPE, LED_PIN_TIER_3, COLOR_ORDER>(leds + 2*MAX_LEDS_PER_TIER, MAX_LEDS_PER_TIER).setCorrection(TypicalLEDStrip);
  FastLED.addLeds<LED_TYPE, LED_PIN_TIER_4, COLOR_ORDER>(leds + 3*MAX_LEDS_PER_TIER, MAX_LEDS_PER_TIER).setCorrection(TypicalLEDStrip);
  FastLED.addLeds<LED_TYPE, LED_PIN_TIER_5, COLOR_ORDER>(leds + 4*MAX_LEDS_PER_TIER, MAX_LEDS_PER_TIER).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(MAX_BRIGHTNESS);
  fill_solid(leds, MAX_LEDS, CRGB::Black);
  FastLED.show();

  // WiFi event callbacks — only set flags, no LED/RTOS calls inside
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    wifiConnected = true;
    statusChanged = true;
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    wifiConnected = false;
    statusChanged = true;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);

  // Wait up to 20 s — continue without WiFi if unavailable (loop() retries).
  // Direct leds[] access is safe here — renderTask hasn't started yet.
  uint8_t dot = 0;
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-wifiStart < 20000) {
    leds[dot % numLeds] = CRGB::Blue; FastLED.show(); delay(300);
    leds[dot % numLeds] = CRGB::Black; dot++; Serial.print(".");
  }
  fill_solid(leds, MAX_LEDS, CRGB::Black); FastLED.show();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("ws",  "tcp", 81);
      MDNS.addService("http","tcp", 80);
      Serial.printf("[mDNS] %s.local ready\n", HOSTNAME);
    }
  } else {
    Serial.println("\n[WiFi] Not connected — will retry in loop.");
  }

  // HTTP + WebSocket
  registerHttpRoutes();
  Serial.println("[HTTP] Port 80 ready.");
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  Serial.println("[WS] Port 81 ready.");

  // renderTask is the sole owner of leds[] from this point on
  xTaskCreatePinnedToCore(renderTask, "render", 4096, NULL, 1, NULL, 0);

  // Show WiFi / mapping status on first 3 LEDs (via queue)
  showSystemStatus();
  statusChanged = false;

  // Ready flash — enqueue green fill, wait, enqueue clear
  if (wifiConnected) {
    LedCommand cmd;
    cmd.type = CMD_FILL; cmd.fill = {0, 255, 0};
    ledEnqueue(cmd);
    delay(400);
    cmd.type = CMD_CLEAR;
    ledEnqueue(cmd);
  }

  Serial.printf("[READY] numLeds=%d  maxLeds=%d\n", numLeds, MAX_LEDS);
}

// ── Loop ──────────────────────────────────────
unsigned long lastWifiCheck = 0;

void loop() {
  httpServer.handleClient();
  webSocket.loop();

  // Periodic WiFi reconnect check
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // Update status LEDs when WiFi or mapping state has changed
  if (statusChanged) {
    statusChanged = false;
    showSystemStatus();
  }
}
