#pragma once
#include <Arduino.h>

// Initialize command handler (optional)
void commandHandlerInit();

// Process a single incoming command line from WebSocket
// Examples:
//   "JOY 0.3 -0.8"
//   "{\"x\":0.3,\"y\":-0.8}"
void commandHandlerHandleLine(const String& line);
