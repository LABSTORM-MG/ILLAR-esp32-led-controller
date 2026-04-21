#include <Arduino.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"
#include "ws_api.h"

extern WebSocketsServer webSocket;

/*
  All commands are JSON text frames. Handlers validate input and enqueue a
  LedCommand — renderTask is the only task that writes leds[] or calls show().

  COLOUR COMMANDS:
  {"cmd":"fill","r":255,"g":0,"b":0}
  {"cmd":"set","index":5,"r":0,"g":255,"b":0}
  {"cmd":"range","from":0,"to":9,"r":0,"g":0,"b":255}
  {"cmd":"multi","leds":[{"i":0,"r":255,"g":0,"b":0},...]}
  {"cmd":"brightness","value":128}
  {"cmd":"clear"}

  EFFECTS:
  {"cmd":"effect","name":"rainbow|chase|blink|test|stop","r":255,"g":0,"b":0,"delay":50}

  LOCATION COMMANDS (requires mapping file):
  {"cmd":"location","name":"A6","r":255,"g":0,"b":0}
  {"cmd":"location","name":"A6","r":255,"g":0,"b":0,"brightness":180}
  {"cmd":"locations","items":[{"name":"A6","r":255,"g":0,"b":0,"brightness":100},...]}
  {"cmd":"locations","items":[...],"brightness":180}
  {"cmd":"location_clear","name":"A6"}

  CONFIGURATION:
  {"cmd":"set_leds","value":60}
  {"cmd":"get_config"}
*/
void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length) {
  if (type != WStype_TEXT) return;

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload, length);

  auto reply = [&](bool ok, const char* extra=nullptr, int ledsLit=-1, int skipped=-1) {
    StaticJsonDocument<256> r;
    r["status"] = ok ? "ok" : "error";
    if (ok) r["cmd"] = doc["cmd"].as<const char*>();
    if (extra) r[ok ? "info" : "msg"] = extra;
    if (ledsLit >= 0) r["leds"] = ledsLit;
    if (skipped > 0)  r["skipped"] = skipped;
    String out; serializeJson(r, out);
    webSocket.sendTXT(clientNum, out);
  };

  if (err == DeserializationError::NoMemory) { reply(false,"message too large"); return; }
  if (err) { reply(false,"JSON parse error"); return; }
  const char* cmd = doc["cmd"];
  if (!cmd) { reply(false,"missing cmd"); return; }

  LedCommand c;

  // ── fill ──────────────────────────────────
  if (strcmp(cmd,"fill")==0) {
    c.type = CMD_FILL;
    c.fill = { (uint8_t)(doc["r"]|0), (uint8_t)(doc["g"]|0), (uint8_t)(doc["b"]|0) };
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true);
  }
  // ── set ───────────────────────────────────
  else if (strcmp(cmd,"set")==0) {
    int idx = doc["index"]|-1;
    if (idx < 0 || idx >= numLeds) { reply(false,"index out of range"); return; }
    c.type = CMD_SET;
    c.set = { (uint16_t)idx, (uint8_t)(doc["r"]|0), (uint8_t)(doc["g"]|0), (uint8_t)(doc["b"]|0) };
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true);
  }
  // ── range ─────────────────────────────────
  else if (strcmp(cmd,"range")==0) {
    c.type = CMD_RANGE;
    c.range = {
      (uint16_t)constrain((int)(doc["from"]|0),        0, numLeds-1),
      (uint16_t)constrain((int)(doc["to"]|(numLeds-1)), 0, numLeds-1),
      (uint8_t)(doc["r"]|0), (uint8_t)(doc["g"]|0), (uint8_t)(doc["b"]|0)
    };
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true);
  }
  // ── multi ─────────────────────────────────
  // Variable-length: fill batchBuffer, enqueue CMD_BATCH_APPLY.
  // batchMutex is held until renderTask releases it after applying.
  else if (strcmp(cmd,"multi")==0) {
    JsonArray leds_arr = doc["leds"].as<JsonArray>();
    if (leds_arr.isNull() || leds_arr.size() == 0) { reply(false,"missing or empty leds"); return; }
    if (xSemaphoreTake(batchMutex, pdMS_TO_TICKS(100)) != pdTRUE) { reply(false,"busy"); return; }
    int count = 0, skipped = 0;
    for (JsonObject item : leds_arr) {
      int idx = item["i"]|-1;
      if (idx >= 0 && idx < numLeds && count < BATCH_BUFFER_SIZE) {
        batchBuffer[count++] = { (uint16_t)idx, CRGB((uint8_t)(item["r"]|0),(uint8_t)(item["g"]|0),(uint8_t)(item["b"]|0)) };
      } else { skipped++; }
    }
    if (count == 0) { xSemaphoreGive(batchMutex); reply(false,"no valid LED indices"); return; }
    c.type = CMD_BATCH_APPLY; c.batch = { count, MODE_IDLE };
    if (!ledEnqueue(c)) { xSemaphoreGive(batchMutex); reply(false,"queue full"); return; }
    reply(true, nullptr, -1, skipped);
  }
  // ── brightness ────────────────────────────
  else if (strcmp(cmd,"brightness")==0) {
    c.type = CMD_BRIGHTNESS;
    c.brightness.value = (uint8_t)constrain((int)(doc["value"]|MAX_BRIGHTNESS), 0, 255);
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true);
  }
  // ── clear ─────────────────────────────────
  else if (strcmp(cmd,"clear")==0) {
    c.type = CMD_CLEAR;
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true);
  }
  // ── effect ────────────────────────────────
  else if (strcmp(cmd,"effect")==0) {
    const char* name = doc["name"];
    if (!name) { reply(false,"missing name"); return; }
    if (strcmp(name,"stop")==0) {
      c.type = CMD_EFFECT_STOP;
    } else {
      Effect eff;
      if      (strcmp(name,"rainbow")==0) eff = EFF_RAINBOW;
      else if (strcmp(name,"chase")  ==0) eff = EFF_CHASE;
      else if (strcmp(name,"blink")  ==0) eff = EFF_BLINK;
      else if (strcmp(name,"test")   ==0) eff = EFF_TEST_WALK;
      else { reply(false,"unknown effect"); return; }
      c.type = CMD_EFFECT_START;
      c.effect = { eff, (uint8_t)(doc["r"]|255), (uint8_t)(doc["g"]|255), (uint8_t)(doc["b"]|255),
                   (uint16_t)(doc["delay"]|50) };
    }
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true);
  }
  // ── location ──────────────────────────────
  // Pre-lookup for validation and leds count; renderTask does the final apply.
  else if (strcmp(cmd,"location")==0) {
    const char* name = doc["name"];
    if (!name) { reply(false,"missing name"); return; }
    const Location* loc = findLocation(name);
    if (!loc) { reply(false,"location not found"); return; }
    uint8_t bright = doc.containsKey("brightness")
      ? (uint8_t)constrain((int)doc["brightness"],0,255) : 255;
    c.type = CMD_LOCATION;
    strncpy(c.location.name, name, MAX_LOCATION_NAME_LEN);
    c.location.name[MAX_LOCATION_NAME_LEN] = '\0';
    c.location.r = (uint16_t)(doc["r"]|0)*bright/255;
    c.location.g = (uint16_t)(doc["g"]|0)*bright/255;
    c.location.b = (uint16_t)(doc["b"]|0)*bright/255;
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true, nullptr, (int)loc->count);
  }
  // ── confirm ───────────────────────────────
  // Green flash on location for 500 ms, then location goes off.
  // Maps to the "withdrawal confirmed" state in the ILLAR colour scheme.
  else if (strcmp(cmd,"confirm")==0) {
    const char* name = doc["name"];
    if (!name) { reply(false,"missing name"); return; }
    const Location* loc = findLocation(name);
    if (!loc) { reply(false,"location not found"); return; }
    c.type = CMD_CONFIRM;
    strncpy(c.confirm.name, name, MAX_LOCATION_NAME_LEN);
    c.confirm.name[MAX_LOCATION_NAME_LEN] = '\0';
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true, nullptr, (int)loc->count);
  }
  // ── locations (batch) ─────────────────────
  // Pre-resolve all names → indices into batchBuffer, then CMD_BATCH_APPLY.
  else if (strcmp(cmd,"locations")==0) {
    JsonArray items = doc["items"].as<JsonArray>();
    if (items.isNull() || items.size() == 0) { reply(false,"missing or empty items"); return; }
    if ((int)items.size() > MAX_BATCH_ITEMS) { reply(false,"too many items"); return; }
    if (xSemaphoreTake(batchMutex, pdMS_TO_TICKS(100)) != pdTRUE) { reply(false,"busy"); return; }
    uint8_t batchBright = doc.containsKey("brightness")
      ? (uint8_t)constrain((int)doc["brightness"],0,255) : 255;
    int bufPos = 0, appliedLeds = 0, missingLocs = 0, invalidItems = 0;
    for (JsonObject item : items) {
      const char* name = item["name"];
      if (!name) { invalidItems++; continue; }
      const Location* loc = findLocation(name);
      if (!loc) { missingLocs++; continue; }
      uint8_t bright = item.containsKey("brightness")
        ? (uint8_t)constrain((int)item["brightness"],0,255) : batchBright;
      uint8_t r = (uint16_t)(item["r"]|0)*bright/255;
      uint8_t g = (uint16_t)(item["g"]|0)*bright/255;
      uint8_t b = (uint16_t)(item["b"]|0)*bright/255;
      for (int i = 0; i < loc->count && bufPos < BATCH_BUFFER_SIZE; i++) {
        uint16_t idx = loc->indices[i];
        if (idx < (uint16_t)numLeds) { batchBuffer[bufPos++] = {idx, CRGB(r,g,b)}; appliedLeds++; }
      }
    }
    if (bufPos == 0) {
      xSemaphoreGive(batchMutex);
      StaticJsonDocument<192> r;
      r["status"]="error"; r["msg"]="no locations matched";
      r["missing_locations"]=missingLocs; r["invalid_items"]=invalidItems;
      String out; serializeJson(r,out); webSocket.sendTXT(clientNum,out); return;
    }
    c.type = CMD_BATCH_APPLY; c.batch = { bufPos, MODE_BATCH };
    if (!ledEnqueue(c)) {
      xSemaphoreGive(batchMutex);
      reply(false,"queue full"); return;
    }
    StaticJsonDocument<192> r;
    r["status"]="ok"; r["cmd"]="locations";
    r["applied_leds"]=appliedLeds; r["missing_locations"]=missingLocs; r["invalid_items"]=invalidItems;
    String out; serializeJson(r,out); webSocket.sendTXT(clientNum,out);
  }
  // ── location_clear ────────────────────────
  else if (strcmp(cmd,"location_clear")==0) {
    const char* name = doc["name"];
    if (!name) { reply(false,"missing name"); return; }
    const Location* loc = findLocation(name);
    if (!loc) { reply(false,"location not found"); return; }
    c.type = CMD_LOCATION_CLEAR;
    strncpy(c.locationClear.name, name, MAX_LOCATION_NAME_LEN);
    c.locationClear.name[MAX_LOCATION_NAME_LEN] = '\0';
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true, nullptr, (int)loc->count);
  }
  // ── mode ──────────────────────────────────
  // Middleware signals which warehouse operation is currently active.
  // Does not change any LEDs — use location/locations commands for that.
  else if (strcmp(cmd,"mode")==0) {
    const char* name = doc["name"];
    if (!name) { reply(false,"missing name"); return; }
    DeviceMode m;
    if      (strcmp(name,"idle")      ==0) m = MODE_IDLE;
    else if (strcmp(name,"inventory") ==0) m = MODE_INVENTORY;
    else if (strcmp(name,"putaway")   ==0) m = MODE_PUTAWAY;
    else if (strcmp(name,"diag")      ==0) m = MODE_DIAG;
    else { reply(false,"unknown mode"); return; }
    c.type = CMD_MODE; c.setMode.mode = m;
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    reply(true);
  }
  // ── set_leds ──────────────────────────────
  else if (strcmp(cmd,"set_leds")==0) {
    int n = doc["value"]|-1;
    if (n < 1 || n > MAX_LEDS) {
      reply(false, String("value must be 1–" + String(MAX_LEDS)).c_str()); return;
    }
    c.type = CMD_SET_LEDS; c.setLeds.value = n;
    if (!ledEnqueue(c)) { reply(false,"queue full"); return; }
    // Reply with the validated target value — renderTask updates numLeds asynchronously.
    StaticJsonDocument<128> r;
    r["status"]="ok"; r["cmd"]="set_leds"; r["num_leds"]=n;
    String out; serializeJson(r,out); webSocket.sendTXT(clientNum,out);
  }
  // ── get_config ────────────────────────────
  else if (strcmp(cmd,"get_config")==0) {
    StaticJsonDocument<128> r;
    r["status"]="ok"; r["cmd"]="get_config";
    r["num_leds"]=numLeds; r["max_leds"]=MAX_LEDS;
    String out; serializeJson(r,out); webSocket.sendTXT(clientNum,out);
  }
  else { reply(false,"unknown command"); }
}
