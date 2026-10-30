#pragma once
#include <Arduino.h>

// Start WiFi + WebSocket server
void wifiWsInit();

// LED + connection status update (optional)
void wifiWsUpdate();

// Send text/JSON to all connected WebSocket clients
bool wifiWsSend(const uint8_t *data, size_t len);
