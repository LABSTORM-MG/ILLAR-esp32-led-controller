#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"
#include "http_api.h"

extern WebServer httpServer;

void addCors() {
  httpServer.sendHeader("Access-Control-Allow-Origin","*");
  httpServer.sendHeader("Access-Control-Allow-Methods","GET,POST,DELETE,OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers","Content-Type");
}

// GET /status
void handleStatus() {
  addCors();
  StaticJsonDocument<256> doc;
  doc["hostname"]   = HOSTNAME;
  doc["ip"]         = WiFi.localIP().toString();
  doc["num_leds"]   = numLeds;
  doc["max_leds"]   = MAX_LEDS;
  doc["locations"]  = (int)locationMap.size();
  doc["mapping_ok"] = LittleFS.exists(MAPPING_FILE);
  doc["mode"]       = (int)currentMode;
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// GET /mapping
void handleGetMapping() {
  addCors();
  if (!LittleFS.exists(MAPPING_FILE)) {
    httpServer.send(404,"application/json","{\"error\":\"no mapping\"}"); return;
  }
  File f = LittleFS.open(MAPPING_FILE,"r");
  httpServer.streamFile(f,"application/json");
  f.close();
}

// POST /upload  — upload location mapping
// Validates schema before writing: root must be object, values must be int or int[].
// Uses atomic write (temp file + rename) to protect against power loss mid-write.
void handleUpload() {
  addCors();
  if (!httpServer.hasArg("plain")) {
    httpServer.send(400,"application/json","{\"error\":\"empty body\"}"); return;
  }
  String body = httpServer.arg("plain");

  DynamicJsonDocument test(8192);
  DeserializationError err = deserializeJson(test, body);
  if (err) {
    httpServer.send(400,"application/json",
      String("{\"error\":\"invalid JSON: ")+err.c_str()+"\"}"); return;
  }
  if (!test.is<JsonObject>()) {
    httpServer.send(400,"application/json",
      "{\"error\":\"mapping must be a JSON object\"}"); return;
  }
  for (JsonPair kv : test.as<JsonObject>()) {
    JsonVariant v = kv.value();
    if (v.is<int>()) continue;
    if (v.is<JsonArray>()) {
      bool valid = true;
      for (JsonVariant elem : v.as<JsonArray>()) {
        if (!elem.is<int>()) { valid = false; break; }
      }
      if (valid) continue;
    }
    httpServer.send(400,"application/json",
      String("{\"error\":\"invalid value for '")+kv.key().c_str()+"': must be int or int[]\"}");
    return;
  }

  // Atomic write: temp file → rename
  File f = LittleFS.open("/mapping.tmp","w");
  if (!f) { httpServer.send(500,"application/json","{\"error\":\"fs write failed\"}"); return; }
  f.print(body); f.close();
  if (!LittleFS.rename("/mapping.tmp", MAPPING_FILE)) {
    httpServer.send(500,"application/json","{\"error\":\"fs rename failed\"}"); return;
  }
  loadMapping();
  statusChanged = true;
  httpServer.send(200,"application/json",
    String("{\"ok\":true,\"locations\":")+((int)locationMap.size())+"}");
}

// DELETE /mapping
void handleDeleteMapping() {
  addCors();
  LittleFS.remove(MAPPING_FILE);
  locationMap.clear();
  statusChanged = true;
  httpServer.send(200,"application/json","{\"ok\":true}");
}

// POST /config  — set num_leds at runtime
// Body: { "num_leds": 60 }
void handleSetConfig() {
  addCors();
  if (!httpServer.hasArg("plain")) {
    httpServer.send(400,"application/json","{\"error\":\"empty body\"}"); return;
  }
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, httpServer.arg("plain"));
  if (err) { httpServer.send(400,"application/json","{\"error\":\"invalid JSON\"}"); return; }
  if (!doc.containsKey("num_leds")) {
    httpServer.send(400,"application/json","{\"error\":\"missing num_leds\"}"); return;
  }
  int n = doc["num_leds"];
  if (n < 1 || n > MAX_LEDS) {
    httpServer.send(400,"application/json",
      String("{\"error\":\"num_leds must be 1–") + MAX_LEDS + "\"}"); return;
  }
  ledAcquire();
  activeEffect = EFF_NONE;
  clearAll();
  numLeds = n;
  ledRelease();
  saveConfig();
  httpServer.send(200,"application/json",String("{\"ok\":true,\"num_leds\":")+numLeds+"}");
  Serial.printf("[HTTP] num_leds set to %d\n", numLeds);
}

// GET /config
void handleGetConfig() {
  addCors();
  StaticJsonDocument<128> doc;
  doc["num_leds"] = numLeds;
  doc["max_leds"] = MAX_LEDS;
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// GET /diag — operational diagnostics for middleware / debugging
void handleDiag() {
  addCors();
  StaticJsonDocument<256> doc;
  doc["heap_free"]    = (int)esp_get_free_heap_size();
  doc["uptime_s"]     = (int)(millis() / 1000);
  doc["wifi_rssi"]    = WiFi.RSSI();
  doc["wifi_status"]  = wifiConnected ? "connected" : "disconnected";
  doc["mapping_locs"] = (int)locationMap.size();
  doc["num_leds"]     = numLeds;
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// GET /test — hardware LED walk test
// Lights each LED white one at a time so wiring can be verified.
// Returns immediately; the walk runs in a background task.
void handleTest() {
  addCors();
  ledAcquire();
  activeEffect = EFF_NONE;
  currentMode  = MODE_DIAG;
  clearAll();
  ledRelease();
  xTaskCreate(testWalkTask, "test_walk", 2048, NULL, 1, NULL);
  httpServer.send(200,"application/json","{\"ok\":true,\"mode\":\"test_running\"}");
}

void handleOptions() { addCors(); httpServer.send(204); }

// Register all HTTP routes and start the server
void registerHttpRoutes() {
  httpServer.on("/status",  HTTP_GET,    handleStatus);
  httpServer.on("/mapping", HTTP_GET,    handleGetMapping);
  httpServer.on("/upload",  HTTP_POST,   handleUpload);
  httpServer.on("/mapping", HTTP_DELETE, handleDeleteMapping);
  httpServer.on("/config",  HTTP_GET,    handleGetConfig);
  httpServer.on("/config",  HTTP_POST,   handleSetConfig);
  httpServer.on("/diag",    HTTP_GET,    handleDiag);
  httpServer.on("/test",    HTTP_GET,    handleTest);
  httpServer.on("/upload",  HTTP_OPTIONS,handleOptions);
  httpServer.on("/mapping", HTTP_OPTIONS,handleOptions);
  httpServer.on("/config",  HTTP_OPTIONS,handleOptions);
  httpServer.begin();
}
