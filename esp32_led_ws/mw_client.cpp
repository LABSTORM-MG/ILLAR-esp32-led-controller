#include <Arduino.h>
#include <WebSocketsClient.h>
#include "config.h"
#include "led_engine.h"
#include "mw_client.h"

/*
  Connects to the Java middleware (Lagerverwaltung) as a WebSocket CLIENT and
  speaks its Tilde text protocol (CMD~KEY:VALUE~KEY:VALUE — see Message /
  MessageParser in de.ross.websocket.protocol). The middleware broadcasts every
  COMMAND message from a browser client to all connected /ws/storage sessions
  (all 3 shelf nodes); renderTask's own findLocation() lookup already ignores
  location names that don't belong to this shelf, so no routing is done here.

  Supported ACTION values (subset of the local JSON API in ws_api.cpp, chosen
  to cover the warehouse pick/confirm/putaway flow):

    COMMAND~ACTION:LOCATION~NAME:A6~R:255~G:255~B:0~BRIGHTNESS:180
    COMMAND~ACTION:CONFIRM~NAME:A6
    COMMAND~ACTION:LOCATION_CLEAR~NAME:A6
    COMMAND~ACTION:CLEAR
    COMMAND~ACTION:FILL~R:0~G:0~B:0

  Fire-and-forget: the middleware only ever consumes PING/PONG replies from
  storage sessions, so no response is sent back here.
*/

static WebSocketsClient mwClient;

// ─── Tiny tilde-protocol parser ───────────────────────────────────
// Mirrors the Java middleware's Message/MessageParser. A handful of key/value
// pairs per command at most, so a small fixed-size array is enough — no map
// or heap allocation needed.
#define MAX_PARAMS 8

struct TildeMessage {
  String cmd;
  String keys[MAX_PARAMS];
  String values[MAX_PARAMS];
  int    paramCount = 0;

  const String* get(const char* key) const {
    for (int i = 0; i < paramCount; i++) {
      if (keys[i].equalsIgnoreCase(key)) return &values[i];
    }
    return nullptr;
  }
};

static TildeMessage parseTilde(const String& raw) {
  TildeMessage msg;
  int firstTilde = raw.indexOf('~');
  msg.cmd = (firstTilde == -1) ? raw : raw.substring(0, firstTilde);

  int start = (firstTilde == -1) ? raw.length() : firstTilde + 1;
  while (start < (int)raw.length() && msg.paramCount < MAX_PARAMS) {
    int next = raw.indexOf('~', start);
    String part = (next == -1) ? raw.substring(start) : raw.substring(start, next);
    int colon = part.indexOf(':');
    if (colon > 0) {
      msg.keys[msg.paramCount]   = part.substring(0, colon);
      msg.values[msg.paramCount] = part.substring(colon + 1);
      msg.paramCount++;
    }
    if (next == -1) break;
    start = next + 1;
  }
  return msg;
}

static uint8_t toByte(const String* s, uint8_t fallback = 0) {
  return s ? (uint8_t)constrain(s->toInt(), 0, 255) : fallback;
}

static void copyName(char* dest, const String& name) {
  strncpy(dest, name.c_str(), MAX_LOCATION_NAME_LEN);
  dest[MAX_LOCATION_NAME_LEN] = '\0';
}

// ─── Dispatch: ACTION param → existing LedCommand queue ───────────
static void handleCommand(const TildeMessage& msg) {
  if (!msg.cmd.equalsIgnoreCase("COMMAND")) return; // ignore ERROR/PONG/etc. echoes

  const String* action = msg.get("ACTION");
  if (!action) return;

  LedCommand c;

  if (action->equalsIgnoreCase("LOCATION")) {
    const String* name = msg.get("NAME");
    if (!name) return;
    uint8_t bright = toByte(msg.get("BRIGHTNESS"), 255);
    c.type = CMD_LOCATION;
    copyName(c.location.name, *name);
    c.location.r = (uint16_t)toByte(msg.get("R")) * bright / 255;
    c.location.g = (uint16_t)toByte(msg.get("G")) * bright / 255;
    c.location.b = (uint16_t)toByte(msg.get("B")) * bright / 255;
    ledEnqueue(c);
  }
  else if (action->equalsIgnoreCase("CONFIRM")) {
    const String* name = msg.get("NAME");
    if (!name) return;
    c.type = CMD_CONFIRM;
    copyName(c.confirm.name, *name);
    ledEnqueue(c);
  }
  else if (action->equalsIgnoreCase("LOCATION_CLEAR")) {
    const String* name = msg.get("NAME");
    if (!name) return;
    c.type = CMD_LOCATION_CLEAR;
    copyName(c.locationClear.name, *name);
    ledEnqueue(c);
  }
  else if (action->equalsIgnoreCase("CLEAR")) {
    c.type = CMD_CLEAR;
    ledEnqueue(c);
  }
  else if (action->equalsIgnoreCase("FILL")) {
    c.type = CMD_FILL;
    c.fill = { toByte(msg.get("R")), toByte(msg.get("G")), toByte(msg.get("B")) };
    ledEnqueue(c);
  }
  else {
    Serial.printf("[MW] unknown ACTION: %s\n", action->c_str());
  }
  // Locations not owned by this shelf are silently ignored inside renderTask's
  // own findLocation() lookup — that's the intended broadcast-to-all behaviour.
}

static void onMwEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[MW] Connected to ws://%s:%u%s\n", MW_HOST, MW_PORT, MW_PATH);
      break;
    case WStype_DISCONNECTED:
      Serial.println("[MW] Disconnected — auto-reconnect running");
      break;
    case WStype_TEXT: {
      String raw = String((char*)payload, length);
      handleCommand(parseTilde(raw));
      break;
    }
    default:
      break;
  }
}

void mwClientBegin() {
  mwClient.begin(MW_HOST, MW_PORT, MW_PATH);
  mwClient.onEvent(onMwEvent);
  mwClient.setReconnectInterval(5000);
  Serial.printf("[MW] Connecting to ws://%s:%u%s ...\n", MW_HOST, MW_PORT, MW_PATH);
}

void mwClientLoop() {
  mwClient.loop();
}
