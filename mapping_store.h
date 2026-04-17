#pragma once
#include <ArduinoJson.h>

// Location map — maps InvenTree location names to LED indices.
// Defined in mapping_store.cpp.
extern DynamicJsonDocument locationMap;

void loadConfig();
void saveConfig();
void loadMapping();
