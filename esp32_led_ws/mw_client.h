#pragma once

// WebSocket client connection to the Java middleware (Lagerverwaltung, /ws/storage).
// Independent of and in addition to the local WebSocketsServer in ws_api.cpp/h —
// that one keeps serving dev_testing_dashboard.html / mapping_tool.html on port 81.

void mwClientBegin();
void mwClientLoop();
