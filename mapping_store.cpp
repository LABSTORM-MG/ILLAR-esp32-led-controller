#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "led_engine.h"
#include "mapping_store.h"

Location locationTable[MAX_MAPPING_LOCATIONS];
int      locationCount = 0;

const Location* findLocation(const char* name) {
  for (int i = 0; i < locationCount; i++) {
    if (strcmp(locationTable[i].name, name) == 0) return &locationTable[i];
  }
  return nullptr;
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) {
    Serial.printf("[FS] No config file — using default numLeds=%d\n", numLeds);
    return;
  }
  File f = LittleFS.open(CONFIG_FILE, "r");
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[FS] Config parse error: %s\n", err.c_str());
    setLastError("config parse error");
    return;
  }
  int n = doc["num_leds"] | numLeds;
  if (n > 0 && n <= MAX_LEDS) {
    numLeds = n;
  } else {
    Serial.printf("[FS] Config num_leds %d out of range (1–%d), using default\n", n, MAX_LEDS);
    setLastError("config num_leds out of range");
  }
  shelfId = doc["shelf_id"] | SHELF_ID_DEFAULT;
  const char* z = doc["zone"] | "";
  strncpy(zone, z, MAX_ZONE_LEN);
  zone[MAX_ZONE_LEN] = '\0';
  Serial.printf("[FS] Config loaded — numLeds=%d  shelfId=%d  zone=%s\n",
                numLeds, shelfId, zone[0] ? zone : "(none)");
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) { Serial.println("[FS] Config write failed"); setLastError("config write failed"); return; }
  StaticJsonDocument<192> doc;
  doc["num_leds"] = numLeds;
  doc["shelf_id"] = shelfId;
  if (zone[0]) doc["zone"] = zone;
  serializeJson(doc, f);
  f.close();
  Serial.printf("[FS] Config saved — numLeds=%d  shelfId=%d  zone=%s\n",
                numLeds, shelfId, zone[0] ? zone : "(none)");
}

void loadMapping() {
  locationCount = 0;
  if (!LittleFS.exists(MAPPING_FILE)) { Serial.println("[FS] No mapping file."); return; }
  File f = LittleFS.open(MAPPING_FILE, "r");
  if (!f) {
    Serial.println("[FS] Mapping open failed.");
    setLastError("mapping open failed");
    return;
  }

  // Use a streaming doc sized for the validated upload limit.
  // ArduinoJson is used only here — never for runtime lookups.
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[FS] Mapping parse error: %s\n", err.c_str());
    setLastError("mapping parse error");
    return;
  }
  if (!doc.is<JsonObject>()) {
    Serial.println("[FS] Mapping root is not an object.");
    setLastError("mapping: root not an object");
    return;
  }

  for (JsonPair kv : doc.as<JsonObject>()) {
    if (locationCount >= MAX_MAPPING_LOCATIONS) break;
    Location& loc = locationTable[locationCount];
    strncpy(loc.name, kv.key().c_str(), MAX_LOCATION_NAME_LEN);
    loc.name[MAX_LOCATION_NAME_LEN] = '\0';
    loc.count = 0;

    JsonVariant v = kv.value();
    if (v.is<JsonArray>()) {
      for (JsonVariant elem : v.as<JsonArray>()) {
        if (loc.count >= MAX_LEDS_PER_LOCATION) break;
        int idx = elem.as<int>();
        if (idx >= 0 && idx < MAX_LEDS) loc.indices[loc.count++] = (uint16_t)idx;
      }
    } else {
      int idx = v.as<int>();
      if (idx >= 0 && idx < MAX_LEDS) { loc.indices[0] = (uint16_t)idx; loc.count = 1; }
    }

    if (loc.count > 0) locationCount++;
  }

  Serial.printf("[FS] Mapping loaded — %d locations.\n", locationCount);
}
