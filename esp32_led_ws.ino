/*
 * ILLAR ESP32-C6 LED Node
 * ========================
 * Edit config.h to set WiFi credentials, hostname, and LED pin before flashing.
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

  // Mutex must exist before effectTask starts
  ledMutex = xSemaphoreCreateMutex();

  // Mount filesystem and load persisted config + mapping
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Mount failed! Using compile-time defaults.");
  } else {
    Serial.println("[FS] Mounted.");
    loadConfig();
    loadMapping();
  }

  // FastLED — register full buffer; active length controlled via numLeds
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, MAX_LEDS)
         .setCorrection(TypicalLEDStrip);
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

  // Wait up to 20 s — continue without WiFi if unavailable (loop() retries)
  uint8_t dot=0;
  unsigned long wifiStart = millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-wifiStart < 20000) {
    leds[dot % numLeds] = CRGB::Blue; FastLED.show(); delay(300);
    leds[dot % numLeds] = CRGB::Black; dot++; Serial.print(".");
  }
  fill_solid(leds, MAX_LEDS, CRGB::Black); FastLED.show();

  if (WiFi.status()==WL_CONNECTED) {
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

  // Effects task — started after mutex is initialised
  xTaskCreatePinnedToCore(effectTask,"effects",4096,NULL,1,&effectTaskHandle,0);

  // Show WiFi / mapping status on first 3 LEDs
  showSystemStatus();
  statusChanged = false;

  // Ready flash (only when WiFi connected)
  if (wifiConnected) {
    ledAcquire();
    fill_solid(leds, numLeds, CRGB::Green); FastLED.show(); delay(400);
    fill_solid(leds, MAX_LEDS, CRGB::Black); FastLED.show();
    ledRelease();
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
