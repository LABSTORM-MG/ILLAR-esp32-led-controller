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
  httpServer.sendHeader("Access-Control-Allow-Methods","GET,POST,PUT,DELETE,OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers","Content-Type");
}

static const char* modeName(DeviceMode m) {
  switch (m) {
    case MODE_IDLE:      return "idle";
    case MODE_PICK:      return "pick";
    case MODE_BATCH:     return "batch";
    case MODE_INVENTORY: return "inventory";
    case MODE_PUTAWAY:   return "putaway";
    case MODE_EFFECT:    return "effect";
    case MODE_DIAG:      return "diag";
    default:             return "unknown";
  }
}

// GET /status
void handleStatus() {
  addCors();
  StaticJsonDocument<320> doc;
  doc["hostname"]   = HOSTNAME;
  doc["shelf_id"]   = shelfId;
  if (zone[0]) doc["zone"] = zone;
  doc["ip"]         = WiFi.localIP().toString();
  doc["num_leds"]   = numLeds;
  doc["max_leds"]   = MAX_LEDS;
  doc["locations"]  = (int)locationCount;
  doc["mapping_ok"] = LittleFS.exists(MAPPING_FILE);
  doc["mode"]       = modeName(currentMode);
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

// PUT /mapping  — upload location mapping (POST /upload kept as alias)
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
    char buf[96]; snprintf(buf, sizeof(buf), "{\"error\":\"invalid JSON: %s\"}", err.c_str());
    httpServer.send(400,"application/json",buf); return;
  }
  if (!test.is<JsonObject>()) {
    httpServer.send(400,"application/json",
      "{\"error\":\"mapping must be a JSON object\"}"); return;
  }
  JsonObject obj = test.as<JsonObject>();
  if ((int)obj.size() > MAX_MAPPING_LOCATIONS) {
    char buf[64]; snprintf(buf, sizeof(buf), "{\"error\":\"too many locations, max %d\"}", MAX_MAPPING_LOCATIONS);
    httpServer.send(400,"application/json",buf); return;
  }
  for (JsonPair kv : obj) {
    if (strlen(kv.key().c_str()) > MAX_LOCATION_NAME_LEN) {
      char buf[80]; snprintf(buf, sizeof(buf), "{\"error\":\"location name too long: '%s'\"}", kv.key().c_str());
      httpServer.send(400,"application/json",buf); return;
    }
    JsonVariant v = kv.value();
    if (v.is<int>()) {
      int idx = v.as<int>();
      if (idx < 0 || idx >= MAX_LEDS) {
        char buf[80]; snprintf(buf, sizeof(buf), "{\"error\":\"index out of range for '%s'\"}", kv.key().c_str());
        httpServer.send(400,"application/json",buf); return;
      }
      continue;
    }
    if (v.is<JsonArray>()) {
      JsonArray arr = v.as<JsonArray>();
      if (arr.size() == 0) {
        char buf[80]; snprintf(buf, sizeof(buf), "{\"error\":\"empty array for '%s'\"}", kv.key().c_str());
        httpServer.send(400,"application/json",buf); return;
      }
      bool valid = true;
      for (JsonVariant elem : arr) {
        if (!elem.is<int>()) { valid = false; break; }
        int idx = elem.as<int>();
        if (idx < 0 || idx >= MAX_LEDS) { valid = false; break; }
      }
      if (valid) continue;
      char buf[80]; snprintf(buf, sizeof(buf), "{\"error\":\"invalid index in array for '%s'\"}", kv.key().c_str());
      httpServer.send(400,"application/json",buf); return;
    }
    char buf[96]; snprintf(buf, sizeof(buf), "{\"error\":\"invalid value for '%s': must be int or int[]\"}", kv.key().c_str());
    httpServer.send(400,"application/json",buf);
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
  char okbuf[48]; snprintf(okbuf, sizeof(okbuf), "{\"ok\":true,\"locations\":%d}", (int)locationCount);
  httpServer.send(200,"application/json",okbuf);
}

// DELETE /mapping
void handleDeleteMapping() {
  addCors();
  LittleFS.remove(MAPPING_FILE);
  locationCount = 0;
  statusChanged = true;
  httpServer.send(200,"application/json","{\"ok\":true}");
}

// POST /config  — set runtime config fields (all optional, save on any change)
// Accepts: { "num_leds": 60, "shelf_id": 2, "zone": "Regal A" }
void handleSetConfig() {
  addCors();
  if (!httpServer.hasArg("plain")) {
    httpServer.send(400,"application/json","{\"error\":\"empty body\"}"); return;
  }
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, httpServer.arg("plain"));
  if (err) { httpServer.send(400,"application/json","{\"error\":\"invalid JSON\"}"); return; }

  bool changed = false;

  if (doc.containsKey("num_leds")) {
    int n = doc["num_leds"];
    if (n < 1 || n > MAX_LEDS) {
      char buf[64]; snprintf(buf, sizeof(buf), "{\"error\":\"num_leds must be 1\\u2013%d\"}", MAX_LEDS);
      httpServer.send(400,"application/json",buf); return;
    }
    LedCommand cmd; cmd.type = CMD_SET_LEDS; cmd.setLeds.value = n;
    if (!ledEnqueue(cmd)) {
      httpServer.send(503,"application/json","{\"error\":\"render queue full\"}"); return;
    }
    changed = true;
  }

  if (doc.containsKey("shelf_id")) {
    int s = doc["shelf_id"];
    if (s < 0 || s > 255) {
      httpServer.send(400,"application/json","{\"error\":\"shelf_id must be 0–255\"}"); return;
    }
    shelfId = s; changed = true;
  }

  if (doc.containsKey("zone")) {
    const char* z = doc["zone"];
    if (strlen(z) > MAX_ZONE_LEN) {
      char buf[64]; snprintf(buf, sizeof(buf), "{\"error\":\"zone too long, max %d\"}", MAX_ZONE_LEN);
      httpServer.send(400,"application/json",buf); return;
    }
    strncpy(zone, z, MAX_ZONE_LEN); zone[MAX_ZONE_LEN] = '\0'; changed = true;
  }

  if (!changed) {
    httpServer.send(400,"application/json","{\"error\":\"no recognised fields\"}"); return;
  }

  saveConfig();

  StaticJsonDocument<192> r;
  r["ok"]       = true;
  r["num_leds"] = numLeds;
  r["shelf_id"] = shelfId;
  r["zone"]     = zone[0] ? zone : "";
  String out; serializeJson(r, out);
  httpServer.send(200,"application/json",out);
}

// GET /config
void handleGetConfig() {
  addCors();
  StaticJsonDocument<192> doc;
  doc["num_leds"] = numLeds;
  doc["max_leds"] = MAX_LEDS;
  doc["shelf_id"] = shelfId;
  doc["zone"]     = zone[0] ? zone : "";
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// GET /diag — operational diagnostics for middleware / debugging
void handleDiag() {
  addCors();
  StaticJsonDocument<384> doc;
  doc["firmware"]     = FIRMWARE_VERSION;
  doc["hostname"]     = HOSTNAME;
  doc["shelf_id"]     = shelfId;
  if (zone[0]) doc["zone"] = zone;
  doc["uptime_s"]     = (int)(millis() / 1000);
  doc["heap_free"]    = (int)esp_get_free_heap_size();
  doc["wifi_status"]  = wifiConnected ? "connected" : "disconnected";
  doc["wifi_rssi"]    = WiFi.RSSI();
  doc["ip"]           = WiFi.localIP().toString();
  doc["num_leds"]     = numLeds;
  doc["max_leds"]     = MAX_LEDS;
  doc["mapping_locs"] = (int)locationCount;
  doc["mode"]         = modeName(currentMode);
  doc["last_error"]   = lastError[0] ? lastError : nullptr;
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// GET /test — hardware LED walk test
// Starts EFF_TEST_WALK in renderTask — no separate task needed.
void handleTest() {
  addCors();
  LedCommand cmd;
  cmd.type = CMD_EFFECT_START;
  cmd.effect = { EFF_TEST_WALK, 0, 0, 0, 30 };
  if (!ledEnqueue(cmd)) {
    httpServer.send(503,"application/json","{\"error\":\"render queue full\"}"); return;
  }
  httpServer.send(200,"application/json","{\"ok\":true,\"mode\":\"test_running\"}");
}

void handleOptions() { addCors(); httpServer.send(204); }

// Register all HTTP routes and start the server
void registerHttpRoutes() {
  httpServer.on("/status",  HTTP_GET,    handleStatus);
  httpServer.on("/mapping", HTTP_GET,    handleGetMapping);
  httpServer.on("/mapping", HTTP_PUT,    handleUpload);   // primary route
  httpServer.on("/upload",  HTTP_POST,   handleUpload);   // legacy alias
  httpServer.on("/mapping", HTTP_DELETE, handleDeleteMapping);
  httpServer.on("/config",  HTTP_GET,    handleGetConfig);
  httpServer.on("/config",  HTTP_POST,   handleSetConfig);
  httpServer.on("/diag",    HTTP_GET,    handleDiag);
  httpServer.on("/test",    HTTP_GET,    handleTest);
  httpServer.on("/mapping", HTTP_OPTIONS,handleOptions);
  httpServer.on("/upload",  HTTP_OPTIONS,handleOptions);
  httpServer.on("/config",  HTTP_OPTIONS,handleOptions);
  httpServer.begin();
}
