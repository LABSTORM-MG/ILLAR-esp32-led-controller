#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"

DynamicJsonDocument locationMap(8192);

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
