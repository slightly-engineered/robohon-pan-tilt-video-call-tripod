#include "wifi_ws.h"
#include "servo_bridge.h"
#include "command_handler.h"
#include "config.h"
#include "webpage.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <ESPmDNS.h>

// ============================================================
// WS2812 Status LED (GPIO 10 on ESP32‑C3 Zero)
// ============================================================
static Adafruit_NeoPixel statusLed(STATUS_LED_COUNT, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

static unsigned long ledTimer = 0;
static bool ledState = false;
static int wsClientCount = 0;

// ============================================================
// WebSocket server
// ============================================================
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ============================================================
// WiFi + WebSocket Init
// ============================================================
void wifiWsInit() {

    // Init WS2812 LED
    statusLed.begin();
    statusLed.setBrightness(50);
    statusLed.clear();
    statusLed.show();

    WiFi.mode(WIFI_STA);

    // Pin a static IP so the board is always reachable at the same
    // address. .111 sits inside the router's DHCP pool (.12-.254 per
    // the Optimum admin page), which risks a conflict if the router
    // later hands that same address to another device. .5 sits
    // outside the pool, so it's safe to self-assign.
    IPAddress staticIP(192, 168, 1, 5);
    IPAddress gateway(192, 168, 1, 1);
    IPAddress subnet(255, 255, 255, 0);
    IPAddress dns(192, 168, 1, 1);

    if (!WiFi.config(staticIP, gateway, subnet, dns)) {
        Serial.println("Static IP config failed - falling back to DHCP.");
    }

    Serial.println("Connecting to WiFi...");

    bool connected = false;

    // Try each SSID in config.h
    for (int i = 0; i < WIFI_COUNT; i++) {

        Serial.printf("Trying SSID: %s\n", WIFI_SSIDS[i]);
        WiFi.begin(WIFI_SSIDS[i], WIFI_PASSWORDS[i]);

        unsigned long start = millis();

        // Try for 6 seconds per network
        while (millis() - start < 6000) {

            if (WiFi.status() == WL_CONNECTED) {
                connected = true;
                break;
            }

            // Fast blink while connecting
            unsigned long now = millis();
            if (now - ledTimer >= 100) {
                ledTimer = now;
                ledState = !ledState;
                if (ledState)
                    statusLed.setPixelColor(0, statusLed.Color(255, 255, 0)); // yellow
                else
                    statusLed.clear();
                statusLed.show();
            }

            delay(50);
            Serial.print(".");
        }

        Serial.println();

        if (connected) break;
    }

    if (!connected) {
        Serial.println("WiFi failed on all networks!");
        return;
    }

    Serial.println("WiFi connected.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP().toString());

    if (MDNS.begin("tank")) {
        Serial.println("mDNS started - reachable at http://tank.local/");
    } else {
        Serial.println("mDNS failed to start - use the IP address instead.");
    }

    // --------------------------------------------------------
    // WebSocket event handler
    // --------------------------------------------------------
    ws.onEvent([](AsyncWebSocket *server,
                  AsyncWebSocketClient *client,
                  AwsEventType type,
                  void *arg,
                  uint8_t *data,
                  size_t len)
    {
        if (type == WS_EVT_CONNECT) {
            wsClientCount++;
            servoBridgeEnable();
            return;
        }

        if (type == WS_EVT_DISCONNECT) {
            if (wsClientCount > 0) wsClientCount--;
            if (wsClientCount == 0) servoBridgeDisable();
            return;
        }

        if (type != WS_EVT_DATA) return;

        AwsFrameInfo *info = (AwsFrameInfo*)arg;

        // ----------------------------------------------------
        // TEXT FRAME → joystick or JSON command
        // ----------------------------------------------------
        if (info->opcode == WS_TEXT) {
            String line = String((char*)data).substring(0, len);
            commandHandlerHandleLine(line);
            return;
        }

        // ----------------------------------------------------
        // Ignore binary frames (not used in tank firmware)
        // ----------------------------------------------------
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", INDEX_HTML);
    });

    server.addHandler(&ws);
    server.begin();

    MDNS.addService("http", "tcp", 80);
}

// ============================================================
// LED + WebSocket update
// ============================================================
void wifiWsUpdate() {
    unsigned long now = millis();

    // WiFi disconnected → LED OFF
    if (WiFi.status() != WL_CONNECTED) {
        statusLed.clear();
        statusLed.show();
        return;
    }

    // Solid green if WS client connected
    if (wsClientCount > 0) {
        statusLed.setPixelColor(0, statusLed.Color(0, 255, 0)); // green
        statusLed.show();
        return;
    }

    // Slow blink (connected but idle)
    if (now - ledTimer >= 500) {
        ledTimer = now;
        ledState = !ledState;

        if (ledState)
            statusLed.setPixelColor(0, statusLed.Color(0, 0, 255)); // blue
        else
            statusLed.clear();

        statusLed.show();
    }
}

// ============================================================
// Send text/JSON to ALL connected WS clients
// ============================================================
bool wifiWsSend(const uint8_t *data, size_t len) {
    ws.textAll((const char*)data, len);
    return true;
}
