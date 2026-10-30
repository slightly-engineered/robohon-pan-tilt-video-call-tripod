#include "command_handler.h"
#include "servo_bridge.h"
#include "wifi_ws.h"
#include <ArduinoJson.h>
#include <Arduino.h>

void commandHandlerInit() {
    // nothing needed yet
}

// ------------------------------------------------------------
// Send JSON response back over WebSocket + Serial
// ------------------------------------------------------------
static void sendResponse(const JsonDocument& doc) {
    String out;
    serializeJson(doc, out);

    // WebSocket
    wifiWsSend((const uint8_t*)out.c_str(), out.length());

    // USB Serial (debug)
    Serial.println(out);
}

// ------------------------------------------------------------
// Handle incoming WebSocket text line
// ------------------------------------------------------------
void commandHandlerHandleLine(const String& line) {

    // --------------------------------------------------------
    // 1) JOY x y  (simple text protocol)
    // --------------------------------------------------------
    if (line.startsWith("JOY")) {
        float x = 0, y = 0;
        sscanf(line.c_str(), "JOY %f %f", &x, &y);

        servoBridgeDrive(x, y);

        StaticJsonDocument<128> resp;
        resp["ok"] = true;
        resp["cmd"] = "joy";
        resp["x"] = x;
        resp["y"] = y;
        sendResponse(resp);
        return;
    }

    // --------------------------------------------------------
    // 2) JSON joystick: {"x":0.3,"y":-0.8}
    // --------------------------------------------------------
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, line);

    if (!err) {
        if (doc.containsKey("x") && doc.containsKey("y")) {
            float x = doc["x"];
            float y = doc["y"];

            servoBridgeDrive(x, y);

            StaticJsonDocument<128> resp;
            resp["ok"] = true;
            resp["cmd"] = "joy";
            resp["x"] = x;
            resp["y"] = y;
            sendResponse(resp);
            return;
        }

        // Unknown JSON command
        StaticJsonDocument<128> resp;
        resp["ok"] = false;
        resp["error"] = "unknown_json_cmd";
        sendResponse(resp);
        return;
    }

    // --------------------------------------------------------
    // 3) Unknown text command
    // --------------------------------------------------------
    StaticJsonDocument<128> resp;
    resp["ok"] = false;
    resp["error"] = "unknown_cmd";
    resp["line"] = line;
    sendResponse(resp);
}
