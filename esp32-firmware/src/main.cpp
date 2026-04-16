/**
 * Phase 1: Network Round-Trip Test
 *
 * Connects to WiFi, sends a test message to the Mac server via HTTP POST,
 * and displays the server's response on the TFT display.
 *
 * Boot 0 button (GPIO 0) triggers each request so you can test on demand.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "config.h"

// ── Display ──────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// ── Button ───────────────────────────────────────────────────
#define BUTTON_PIN 0  // Boot button on most ESP32-S3 boards
volatile bool buttonPressed = false;

void IRAM_ATTR onButtonPress() {
    buttonPressed = true;
}

// ── Display helpers ──────────────────────────────────────────
void displayStatus(const char* line1, const char* line2 = "", uint16_t color = TFT_WHITE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextFont(2);

    tft.setCursor(4, 10);
    tft.println(line1);

    if (strlen(line2) > 0) {
        tft.setCursor(4, 35);
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.println(line2);
    }
}

void displayResponse(const char* label, const char* body) {
    tft.fillScreen(TFT_BLACK);

    // Header
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextFont(2);
    tft.setCursor(4, 4);
    tft.println(label);

    // Divider line
    tft.drawFastHLine(0, 22, 128, TFT_DARKGREY);

    // Body text - word-wrapped
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(1);
    tft.setCursor(4, 28);
    tft.setTextWrap(true);
    tft.print(body);

    // Footer
    tft.drawFastHLine(0, 148, 128, TFT_DARKGREY);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(4, 150);
    tft.print("BTN -> send again");
}

// ── WiFi ─────────────────────────────────────────────────────
void connectWiFi() {
    displayStatus("Connecting WiFi...", WIFI_SSID, TFT_YELLOW);
    Serial.printf("Connecting to %s", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
        char ipBuf[40];
        snprintf(ipBuf, sizeof(ipBuf), "IP: %s", WiFi.localIP().toString().c_str());
        displayStatus("WiFi Connected", ipBuf, TFT_GREEN);
        delay(1500);
    } else {
        Serial.println("\nWiFi FAILED");
        displayStatus("WiFi FAILED", "Check config.h", TFT_RED);
        while (true) { delay(1000); }  // halt
    }
}

// ── HTTP request to Mac server ───────────────────────────────
static int requestCount = 0;

void sendTestRequest() {
    requestCount++;
    displayStatus("Sending...", "POST to Mac server", TFT_YELLOW);

    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/test", SERVER_HOST, SERVER_PORT);

    Serial.printf("\n[%d] POST %s\n", requestCount, url);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    // Build JSON payload
    JsonDocument doc;
    doc["message"] = "Hello from ESP32!";
    doc["request_id"] = requestCount;
    doc["uptime_ms"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();

    String payload;
    serializeJson(doc, payload);
    Serial.printf("Payload: %s\n", payload.c_str());

    int httpCode = http.POST(payload);

    if (httpCode > 0) {
        String response = http.getString();
        Serial.printf("HTTP %d: %s\n", httpCode, response.c_str());

        if (httpCode == 200) {
            // Parse response JSON
            JsonDocument resDoc;
            DeserializationError err = deserializeJson(resDoc, response);

            if (!err && resDoc["display_text"].is<const char*>()) {
                const char* displayText = resDoc["display_text"];
                char header[32];
                snprintf(header, sizeof(header), "Response #%d", requestCount);
                displayResponse(header, displayText);
            } else {
                // Show raw response if not JSON
                char header[32];
                snprintf(header, sizeof(header), "Raw #%d (HTTP %d)", requestCount, httpCode);
                displayResponse(header, response.c_str());
            }
        } else {
            char errMsg[64];
            snprintf(errMsg, sizeof(errMsg), "HTTP %d", httpCode);
            displayStatus("Server Error", errMsg, TFT_RED);
        }
    } else {
        Serial.printf("Request failed: %s\n", http.errorToString(httpCode).c_str());
        char errMsg[64];
        snprintf(errMsg, sizeof(errMsg), "%s", http.errorToString(httpCode).c_str());
        displayStatus("Request Failed", errMsg, TFT_RED);
    }

    http.end();
}

// ── Setup & Loop ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Pony P1 - Phase 1: Network Test ===");

    // Init display
    tft.init();
    tft.setRotation(1);  // Landscape: 160x128
    tft.fillScreen(TFT_BLACK);
    displayStatus("Pony P1", "Booting...", TFT_CYAN);
    delay(500);

    // Init button
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonPress, FALLING);

    // Connect WiFi
    connectWiFi();

    // Send initial test request
    sendTestRequest();
}

void loop() {
    if (buttonPressed) {
        buttonPressed = false;
        delay(200);  // simple debounce
        sendTestRequest();
    }

    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, reconnecting...");
        displayStatus("WiFi lost", "Reconnecting...", TFT_YELLOW);
        connectWiFi();
    }

    delay(50);
}
