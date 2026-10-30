#include <Arduino.h>
#include "wifi_ws.h"
#include "command_handler.h"
#include "servo_bridge.h"

void setup() {
    Serial.begin(115200);
    delay(500);

    commandHandlerInit();
    wifiWsInit();

    Serial.println("Tank firmware started.");
}

void loop() {
    wifiWsUpdate();     // LED + WS status
    servoBridgePoll();  // integrates joystick -> setpoint, sends bus frames
    delay(10);
}
