#pragma once
#include <WebSocketsServer.h>

void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t length);
