#include <Arduino.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"
#include "ws_api.h"

extern WebSocketsServer webSocket;

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
    if (extra) r[ok?"info":"msg"] = extra;
    if (ledsLit >= 0) r["leds"] = ledsLit;
    if (skipped > 0)  r["skipped"] = skipped;
    String out; serializeJson(r, out);
    webSocket.sendTXT(clientNum, out);
  };

  if (err == DeserializationError::NoMemory) { reply(false,"message too large"); return; }
  if (err) { reply(false,"JSON parse error"); return; }
  const char* cmd = doc["cmd"];
  if (!cmd) { reply(false,"missing cmd"); return; }

  // ── fill ──────────────────────────────────
  if (strcmp(cmd,"fill")==0) {
    ledAcquire();
    activeEffect=EFF_NONE; currentMode=MODE_IDLE;
    setAll(doc["r"]|0,doc["g"]|0,doc["b"]|0);
    ledRelease();
    reply(true);
  }
  // ── set ───────────────────────────────────
  else if (strcmp(cmd,"set")==0) {
    int idx=doc["index"]|-1;
    if (idx<0||idx>=numLeds){reply(false,"index out of range");return;}
    ledAcquire();
    activeEffect=EFF_NONE; currentMode=MODE_IDLE;
    leds[idx]=CRGB(doc["r"]|0,doc["g"]|0,doc["b"]|0); FastLED.show();
    ledRelease();
    reply(true);
  }
  // ── range ─────────────────────────────────
  else if (strcmp(cmd,"range")==0) {
    ledAcquire();
    activeEffect=EFF_NONE; currentMode=MODE_IDLE;
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
    activeEffect=EFF_NONE; currentMode=MODE_IDLE;
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
    activeEffect=EFF_NONE; currentMode=MODE_IDLE; clearAll();
    ledRelease();
    reply(true);
  }
  // ── effect ────────────────────────────────
  else if (strcmp(cmd,"effect")==0) {
    const char* name=doc["name"];
    if(!name){reply(false,"missing name");return;}
    ledAcquire();
    effR=doc["r"]|255; effG=doc["g"]|255; effB=doc["b"]|255; effDelay=doc["delay"]|50;
    if      (strcmp(name,"rainbow")==0) { activeEffect=EFF_RAINBOW; currentMode=MODE_EFFECT; }
    else if (strcmp(name,"chase")  ==0) { activeEffect=EFF_CHASE;   currentMode=MODE_EFFECT; }
    else if (strcmp(name,"blink")  ==0) { activeEffect=EFF_BLINK;   currentMode=MODE_EFFECT; }
    else if (strcmp(name,"stop")   ==0) { activeEffect=EFF_NONE; currentMode=MODE_IDLE; clearAll(); }
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
    activeEffect=EFF_NONE; currentMode=MODE_PICK;
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
  // All items written to buffer first, then a single FastLED.show().
  // "brightness" is one optional value for the whole batch.
  else if (strcmp(cmd,"locations")==0) {
    JsonArray items=doc["items"].as<JsonArray>();
    if(items.isNull()){reply(false,"missing items");return;}
    uint8_t bright=doc.containsKey("brightness")
      ?(uint8_t)constrain((int)doc["brightness"],0,255)
      :FastLED.getBrightness();
    ledAcquire();
    activeEffect=EFF_NONE; currentMode=MODE_BATCH;
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
      reply(false, String("value must be 1–" + String(MAX_LEDS)).c_str()); return;
    }
    ledAcquire();
    activeEffect = EFF_NONE; currentMode = MODE_IDLE;
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
