#pragma once
#include <Arduino.h>
#include "config.h"

// Internal representation of one named location → LED index list.
// JSON is only used during upload/load; runtime lookups use this struct array.
struct Location {
  char     name[MAX_LOCATION_NAME_LEN + 1];
  uint16_t indices[MAX_LEDS_PER_LOCATION];
  uint8_t  count;
};

extern Location locationTable[];
extern int      locationCount;

// Returns a pointer to the Location entry with the given name, or nullptr.
const Location* findLocation(const char* name);

void loadConfig();
void saveConfig();
void loadMapping();   // parses MAPPING_FILE and populates locationTable
