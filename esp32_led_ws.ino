/*
 * ESP32-C6 Addressable RGB LED WebSocket Controller
 * With LittleFS location mapping + HTTP upload server
 * + Runtime-configurable LED count
 * ====================================================
 *
 * Libraries required (Arduino Library Manager):
 *   - FastLED           (by Daniel Garcia)
 *   - WebSockets        (by Markus Sattler)
 *   - ArduinoJson       (by Benoit Blanchon, v6)
 *   - ESPmDNS           (bundled with ESP32 core)
 *
 * Filesystem: LittleFS (bundled with ESP32 core >= 2.0.0)
 *
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

// ─────────────────────────────────────────────
//  USER CONFIGURATION  (edit these per device)
// ─────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
const char* HOSTNAME      = "led-node-1";   // change per device

#define LED_PIN         8
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define MAX_BRIGHTNESS  255

// Maximum number of LEDs this device will ever drive.
// The actual active count is loaded from flash at boot (set via HTTP or WS).
// Increase if your strip is larger — uses 3 bytes of RAM per LED.
#define MAX_LEDS        500

#define CONFIG_FILE    "/config.json"
#define MAPPING_FILE   "/mapping.json"
// ─────────────────────────────────────────────

// LED buffer — full capacity allocated at compile time
CRGB leds[MAX_LEDS];

// Mutex — all writes to leds[] and FastLED.show() must hold this lock.
// Prevents races between effectTask (FreeRTOS task) and WS/HTTP handlers (main loop).
// lightLocation(), setAll(), clearAll() do NOT acquire internally — callers must hold it.
SemaphoreHandle_t ledMutex = NULL;
inline void ledAcquire() { xSemaphoreTake(ledMutex, portMAX_DELAY); }
inline void ledRelease() { xSemaphoreGive(ledMutex); }

// Runtime LED count — loaded from flash, changeable without reflashing
int numLeds = 30;   // sensible default if no config file exists yet

// WiFi state — set by WiFi event callbacks (flag only, no RTOS calls in callbacks)
volatile bool wifiConnected = false;
bool statusChanged = false;  // triggers showSystemStatus() in loop()

WebSocketsServer webSocket(81);
WebServer        httpServer(80);

// ── Config (num_leds stored in flash) ─────────
void loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) {
    Serial.printf("[FS] No config file — using default numLeds=%d\n", numLeds);
    return;
  }
  File f = LittleFS.open(CONFIG_FILE, "r");
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.printf("[FS] Config parse error: %s\n", err.c_str()); return; }
  int n = doc["num_leds"] | numLeds;
  if (n > 0 && n <= MAX_LEDS) {
    numLeds = n;
    Serial.printf("[FS] Config loaded — numLeds=%d\n", numLeds);
  } else {
    Serial.printf("[FS] Config num_leds %d out of range (1–%d), using default\n", n, MAX_LEDS);
  }
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) { Serial.println("[FS] Config write failed"); return; }
  StaticJsonDocument<128> doc;
  doc["num_leds"] = numLeds;
  serializeJson(doc, f);
  f.close();
  Serial.printf("[FS] Config saved — numLeds=%d\n", numLeds);
}

// ── Location map ─────────────────────────────
DynamicJsonDocument locationMap(8192);

void loadMapping() {
  locationMap.clear();
  if (!LittleFS.exists(MAPPING_FILE)) { Serial.println("[FS] No mapping file."); return; }
  File f = LittleFS.open(MAPPING_FILE, "r");
  if (!f) { Serial.println("[FS] Mapping open failed."); return; }
  DeserializationError err = deserializeJson(locationMap, f);
  f.close();
  if (err) { Serial.printf("[FS] Mapping parse error: %s\n", err.c_str()); locationMap.clear(); }
  else     { Serial.printf("[FS] Mapping loaded — %d locations.\n", (int)locationMap.size()); }
}

// Writes location LEDs into buffer only — no FastLED.show(), no brightness change.
// Caller must hold ledMutex and call FastLED.show() when ready.
// Returns number of LEDs written, or -1 if location not found.
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

// ── Helpers — caller must hold ledMutex ───────
void setAll(uint8_t r, uint8_t g, uint8_t b) {
  fill_solid(leds, numLeds, CRGB(r,g,b)); FastLED.show();
}
void clearAll() {
  // Fills entire MAX_LEDS buffer (handles LED count reductions) then shows
  fill_solid(leds, MAX_LEDS, CRGB::Black);
  FastLED.show();
}

// ── System status indicator ───────────────────
// First 3 LEDs show operational state — red = no WiFi, orange = no mapping, off = all good.
// Call only from main loop, not from effectTask.
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

// ── Effects ───────────────────────────────────
enum Effect { EFF_NONE, EFF_RAINBOW, EFF_CHASE, EFF_BLINK };
Effect   activeEffect = EFF_NONE;
uint8_t  effR=255, effG=255, effB=255;
uint16_t effDelay=50;
TaskHandle_t effectTaskHandle = NULL;

void effectTask(void* pv) {
  uint8_t hue=0; uint16_t pos=0; bool blinkOn=false;
  while (true) {
    ledAcquire();
    Effect    eff      = activeEffect;  // local copy while locked
    uint16_t  delay_ms = effDelay;      // local copy — used after release
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

// ── HTTP handlers ─────────────────────────────
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
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// GET /mapping
void handleGetMapping() {
  addCors();
  if (!LittleFS.exists(MAPPING_FILE)) { httpServer.send(404,"application/json","{\"error\":\"no mapping\"}"); return; }
  File f = LittleFS.open(MAPPING_FILE,"r");
  httpServer.streamFile(f,"application/json");
  f.close();
}

// POST /upload  — upload location mapping
// Validates schema before writing: root must be object, values must be int or int[].
// Uses atomic write (temp file + rename) to protect against power loss mid-write.
void handleUpload() {
  addCors();
  if (!httpServer.hasArg("plain")) { httpServer.send(400,"application/json","{\"error\":\"empty body\"}"); return; }
  String body = httpServer.arg("plain");

  DynamicJsonDocument test(8192);
  DeserializationError err = deserializeJson(test, body);
  if (err) { httpServer.send(400,"application/json",String("{\"error\":\"invalid JSON: ")+err.c_str()+"\"}"); return; }

  // Root must be a JSON object
  if (!test.is<JsonObject>()) {
    httpServer.send(400,"application/json","{\"error\":\"mapping must be a JSON object\"}"); return;
  }
  // Each value must be int or int[]
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
      String("{\"error\":\"invalid value for '") + kv.key().c_str() + "': must be int or int[]\"}");
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
  httpServer.send(200,"application/json",String("{\"ok\":true,\"locations\":")+((int)locationMap.size())+"}");
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
// Takes effect immediately, no reboot needed.
void handleSetConfig() {
  addCors();
  if (!httpServer.hasArg("plain")) { httpServer.send(400,"application/json","{\"error\":\"empty body\"}"); return; }
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, httpServer.arg("plain"));
  if (err) { httpServer.send(400,"application/json","{\"error\":\"invalid JSON\"}"); return; }
  if (!doc.containsKey("num_leds")) { httpServer.send(400,"application/json","{\"error\":\"missing num_leds\"}"); return; }
  int n = doc["num_leds"];
  if (n < 1 || n > MAX_LEDS) {
    httpServer.send(400,"application/json",
      String("{\"error\":\"num_leds must be 1–") + MAX_LEDS + "\"}");
    return;
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

void handleOptions() { addCors(); httpServer.send(204); }

// ── WebSocket handler ─────────────────────────
/*
  All commands are JSON text frames.

  COLOUR COMMANDS:
  {"cmd":"fill","r":255,"g":0,"b":0}
  {"cmd":"set","index":5,"r":0,"g":255,"b":0}
  {"cmd":"range","from":0,"to":9,"r":0,"g":0,"b":255}
  {"cmd":"multi","leds":[{"i":0,"r":255,"g":0,"b":0},...]}
  {"cmd":"brightness","value":128}
  {"cmd":"clear"}

  EFFECTS:
  {"cmd":"effect","name":"rainbow|chase|blink|stop","r":255,"g":0,"b":0,"delay":50}

  LOCATION COMMANDS (requires mapping file):
  {"cmd":"location","name":"A6","r":255,"g":0,"b":0}
  {"cmd":"location","name":"A6","r":255,"g":0,"b":0,"brightness":180}
  {"cmd":"locations","items":[{"name":"A6","r":255,"g":0,"b":0},...]}
  {"cmd":"locations","items":[...],"brightness":180}
    Note: brightness in a locations batch is a single value for the whole batch.
  {"cmd":"location_clear","name":"A6"}

  CONFIGURATION:
  {"cmd":"set_leds","value":60}        <- set active LED count (1–MAX_LEDS), saved to flash
  {"cmd":"get_config"}                 <- returns {"status":"ok","cmd":"get_config","num_leds":60,"max_leds":500}
*/
void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  if (type != WStype_TEXT) return;

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload, length);

  auto reply = [&](bool ok, const char* extra=nullptr, int ledsLit=-1, int skipped=-1) {
    StaticJsonDocument<256> r;
    r["status"] = ok ? "ok" : "error";
    if (ok) r["cmd"] = doc["cmd"].as<const char*>();
    if (extra) r[ok?"info":"msg"] = extra;
    if (ledsLit >= 0) r["leds"] = ledsLit;
    if (skipped > 0)  r["skipped"] = skipped;
    String out; serializeJson(r, out);
    webSocket.sendTXT(clientNum, out);
  };

  if (err == DeserializationError::NoMemory) { reply(false,"message too large"); return; }
  if (err) { reply(false,"JSON parse error"); return; }
  const char* cmd = doc["cmd"];
  if (!cmd)  { reply(false,"missing cmd"); return; }

  // ── fill ──────────────────────────────────
  if (strcmp(cmd,"fill")==0) {
    ledAcquire();
    activeEffect=EFF_NONE; setAll(doc["r"]|0,doc["g"]|0,doc["b"]|0);
    ledRelease();
    reply(true);
  }
  // ── set ───────────────────────────────────
  else if (strcmp(cmd,"set")==0) {
    int idx=doc["index"]|-1;
    if (idx<0||idx>=numLeds){reply(false,"index out of range");return;}
    ledAcquire();
    activeEffect=EFF_NONE;
    leds[idx]=CRGB(doc["r"]|0,doc["g"]|0,doc["b"]|0); FastLED.show();
    ledRelease();
    reply(true);
  }
  // ── range ─────────────────────────────────
  else if (strcmp(cmd,"range")==0) {
    ledAcquire();
    activeEffect=EFF_NONE;
    int from=constrain((int)(doc["from"]|0),0,numLeds-1);
    int to  =constrain((int)(doc["to"]|(numLeds-1)),0,numLeds-1);
    CRGB color(doc["r"]|0,doc["g"]|0,doc["b"]|0);
    for(int i=from;i<=to;i++) leds[i]=color;
    FastLED.show();
    ledRelease();
    reply(true);
  }
  // ── multi ─────────────────────────────────
  else if (strcmp(cmd,"multi")==0) {
    ledAcquire();
    activeEffect=EFF_NONE;
    int total=0, skipped=0;
    for(JsonObject item : doc["leds"].as<JsonArray>()) {
      int idx=item["i"]|-1;
      if(idx>=0&&idx<numLeds) { leds[idx]=CRGB(item["r"]|0,item["g"]|0,item["b"]|0); total++; }
      else skipped++;
    }
    if(total>0) FastLED.show();
    ledRelease();
    if(total==0){reply(false,"no valid LED indices");return;}
    reply(true,nullptr,-1,skipped);
  }
  // ── brightness ────────────────────────────
  else if (strcmp(cmd,"brightness")==0) {
    ledAcquire();
    FastLED.setBrightness(constrain((int)(doc["value"]|MAX_BRIGHTNESS),0,255));
    FastLED.show();
    ledRelease();
    reply(true);
  }
  // ── clear ─────────────────────────────────
  else if (strcmp(cmd,"clear")==0) {
    ledAcquire();
    activeEffect=EFF_NONE; clearAll();
    ledRelease();
    reply(true);
  }
  // ── effect ────────────────────────────────
  else if (strcmp(cmd,"effect")==0) {
    const char* name=doc["name"];
    if(!name){reply(false,"missing name");return;}
    ledAcquire();
    effR=doc["r"]|255; effG=doc["g"]|255; effB=doc["b"]|255; effDelay=doc["delay"]|50;
    if      (strcmp(name,"rainbow")==0) activeEffect=EFF_RAINBOW;
    else if (strcmp(name,"chase")  ==0) activeEffect=EFF_CHASE;
    else if (strcmp(name,"blink")  ==0) activeEffect=EFF_BLINK;
    else if (strcmp(name,"stop")   ==0) { activeEffect=EFF_NONE; clearAll(); }
    else { ledRelease(); reply(false,"unknown effect"); return; }
    ledRelease();
    reply(true);
  }
  // ── location ──────────────────────────────
  else if (strcmp(cmd,"location")==0) {
    const char* name=doc["name"];
    if(!name){reply(false,"missing name");return;}
    uint8_t bright=doc.containsKey("brightness")
      ?(uint8_t)constrain((int)doc["brightness"],0,255)
      :FastLED.getBrightness();
    ledAcquire();
    activeEffect=EFF_NONE;
    uint8_t saved=FastLED.getBrightness();
    FastLED.setBrightness(bright);
    int n=lightLocation(name,doc["r"]|0,doc["g"]|0,doc["b"]|0);
    if(n>0) FastLED.show();
    FastLED.setBrightness(saved);
    ledRelease();
    if(n<0){reply(false,"location not found");return;}
    reply(true,nullptr,n);
  }
  // ── locations (batch) ─────────────────────
  // All items are written to the buffer first, then a single FastLED.show() is called.
  // "brightness" is a single optional value for the whole batch.
  else if (strcmp(cmd,"locations")==0) {
    JsonArray items=doc["items"].as<JsonArray>();
    if(items.isNull()){reply(false,"missing items");return;}
    uint8_t bright=doc.containsKey("brightness")
      ?(uint8_t)constrain((int)doc["brightness"],0,255)
      :FastLED.getBrightness();
    ledAcquire();
    activeEffect=EFF_NONE;
    uint8_t saved=FastLED.getBrightness();
    FastLED.setBrightness(bright);
    int total=0, skipped=0;
    for(JsonObject item:items){
      const char* name=item["name"];
      if(!name){skipped++;continue;}
      int n=lightLocation(name,item["r"]|0,item["g"]|0,item["b"]|0);
      if(n>0) total+=n; else skipped++;
    }
    if(total>0) FastLED.show();
    FastLED.setBrightness(saved);
    ledRelease();
    if(total==0){reply(false,"no locations matched");return;}
    reply(true,nullptr,total,skipped);
  }
  // ── location_clear ────────────────────────
  else if (strcmp(cmd,"location_clear")==0) {
    const char* name=doc["name"];
    if(!name){reply(false,"missing name");return;}
    ledAcquire();
    activeEffect=EFF_NONE;
    int n=lightLocation(name,0,0,0);
    if(n>0) FastLED.show();
    ledRelease();
    if(n<0){reply(false,"location not found");return;}
    reply(true,nullptr,n);
  }
  // ── set_leds ──────────────────────────────
  else if (strcmp(cmd,"set_leds")==0) {
    int n = doc["value"]|-1;
    if (n < 1 || n > MAX_LEDS) {
      reply(false, String("value must be 1–" + String(MAX_LEDS)).c_str());
      return;
    }
    ledAcquire();
    activeEffect = EFF_NONE;
    clearAll();
    numLeds = n;
    ledRelease();
    saveConfig();
    StaticJsonDocument<128> r;
    r["status"]   = "ok";
    r["cmd"]      = "set_leds";
    r["num_leds"] = numLeds;
    String out; serializeJson(r, out);
    webSocket.sendTXT(clientNum, out);
  }
  // ── get_config ────────────────────────────
  else if (strcmp(cmd,"get_config")==0) {
    StaticJsonDocument<128> r;
    r["status"]   = "ok";
    r["cmd"]      = "get_config";
    r["num_leds"] = numLeds;
    r["max_leds"] = MAX_LEDS;
    String out; serializeJson(r, out);
    webSocket.sendTXT(clientNum, out);
  }
  else { reply(false,"unknown command"); }
}

// ── Setup ─────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  // Mutex must exist before effectTask starts
  ledMutex = xSemaphoreCreateMutex();

  // Mount filesystem first so we can load numLeds before FastLED init
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Mount failed! Using compile-time defaults.");
  } else {
    Serial.println("[FS] Mounted.");
    loadConfig();
    loadMapping();
  }

  // FastLED — register the full buffer; active length is controlled via numLeds
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

  // HTTP routes
  httpServer.on("/status",  HTTP_GET,    handleStatus);
  httpServer.on("/mapping", HTTP_GET,    handleGetMapping);
  httpServer.on("/upload",  HTTP_POST,   handleUpload);
  httpServer.on("/mapping", HTTP_DELETE, handleDeleteMapping);
  httpServer.on("/config",  HTTP_GET,    handleGetConfig);
  httpServer.on("/config",  HTTP_POST,   handleSetConfig);
  httpServer.on("/diag",    HTTP_GET,    handleDiag);
  httpServer.on("/upload",  HTTP_OPTIONS,handleOptions);
  httpServer.on("/mapping", HTTP_OPTIONS,handleOptions);
  httpServer.on("/config",  HTTP_OPTIONS,handleOptions);
  httpServer.begin();
  Serial.println("[HTTP] Port 80 ready.");

  // WebSocket
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
