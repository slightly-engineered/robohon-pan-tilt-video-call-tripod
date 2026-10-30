#pragma once
#include <Arduino.h>

void servoBridgeEnable();
void servoBridgeDisable();
bool servoBridgeActive();
void servoBridgeDrive(float x, float y);
void servoBridgePoll();
