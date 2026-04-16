/**
 * Pony P1 - Phase 1: Network Round-Trip Test (Arduino IDE version)
 *
 * Connects to WiFi, sends a test message to the Mac server via HTTP POST,
 * and displays the server's response on the ST7735S TFT display.
 *
 * Boot 0 button (GPIO 0) triggers each request.
 *
 * ── Arduino IDE Setup ────────────────────────────────────────
 *
 * 1. Board Manager:
 *    File > Preferences > Additional Board Manager URLs, add:
 *      https://espressif.github.io/arduino-esp32/package_esp32_index.json
 *    Then: Tools > Board > Boards Manager > search "esp32" > install "esp32 by Espressif"
 *
 * 2. Install Libraries (Tools > Manage Libraries):
 *    - "Adafruit GFX Library" by Adafruit
 *    - "Adafruit ST7735 and ST7789 Library" by Adafruit
 *    - "ArduinoJson" by Benoit Blanchon
 *
 * 3. Board Settings (Tools menu):
 *    - Board:            "ESP32S3 Dev Module"
 *    - USB CDC On Boot:  "Enabled"
 *    - Upload Speed:     921600
 *    - Flash Size:       "16MB (128Mb)" (or whatever your T7-S3 has)
 *    - PSRAM:            "OPI PSRAM"
 *    - Port:             (select the COM/tty port that appears when you plug in)
 *
 * 4. Edit config.h with your WiFi credentials and Mac IP.
 *
 * 5. Upload!
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include "config.h"

// ── Display ──────────────────────────────────────────────────
// Use hardware SPI on custom pins via ESP32's flexible SPI
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);

// ── Button ───────────────────────────────────────────────────
#define BUTTON_PIN 0  // Boot button on most ESP32-S3 boards
volatile bool buttonPressed = false;

void IRAM_ATTR onButtonPress() {
    buttonPressed = true;
}

// ── Color definitions (ST7735 uses BGR565) ───────────────────
#define COLOR_BLACK    ST77XX_BLACK
#define COLOR_WHITE    ST77XX_WHITE
#define COLOR_CYAN     ST77XX_CYAN
#define COLOR_YELLOW   ST77XX_YELLOW
#define COLOR_GREEN    ST77XX_GREEN
#define COLOR_RED      ST77XX_RED
#define COLOR_GREY     0x7BEF
#define COLOR_DARKGREY 0x4208

// ── Display helpers ──────────────────────────────────────────
void displayStatus(const char* line1, const char* line2, uint16_t color) {
    tft.fillScreen(COLOR_BLACK);
    tft.setTextColor(color);
    tft.setTextSize(1);
    tft.setCursor(4, 10);
    tft.println(line1);

    if (strlen(line2) > 0) {
        tft.setTextColor(COLOR_GREY);
        tft.setCursor(4, 28);
        tft.println(line2);
    }
}

void displayResponse(const char* label, const char* body) {
    tft.fillScreen(COLOR_BLACK);

    // Header
    tft.setTextColor(COLOR_CYAN);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.println(label);

    // Divider line
    tft.drawFastHLine(0, 16, tft.width(), COLOR_DARKGREY);

    // Body text - word-wrapped
    tft.setTextColor(COLOR_WHITE);
    tft.setCursor(4, 22);
    tft.setTextWrap(true);
    tft.print(body);

    // Footer
    tft.drawFastHLine(0, tft.height() - 12, tft.width(), COLOR_DARKGREY);
    tft.setTextColor(COLOR_DARKGREY);
    tft.setCursor(4, tft.height() - 10);
    tft.print("BTN -> send again");
}

// ── WiFi ─────────────────────────────────────────────────────
void connectWiFi() {
    displayStatus("Connecting WiFi...", WIFI_SSID, COLOR_YELLOW);
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
        displayStatus("WiFi Connected", ipBuf, COLOR_GREEN);
        delay(1500);
    } else {
        Serial.println("\nWiFi FAILED");
        displayStatus("WiFi FAILED", "Check config.h", COLOR_RED);
        while (true) { delay(1000); }  // halt
    }
}

// ── HTTP request to Mac server ───────────────────────────────
static int requestCount = 0;

void sendTestRequest() {
    requestCount++;
    displayStatus("Sending...", "POST to Mac server", COLOR_YELLOW);

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
            displayStatus("Server Error", errMsg, COLOR_RED);
        }
    } else {
        Serial.printf("Request failed: %s\n", http.errorToString(httpCode).c_str());
        char errMsg[64];
        snprintf(errMsg, sizeof(errMsg), "%s", http.errorToString(httpCode).c_str());
        displayStatus("Request Failed", errMsg, COLOR_RED);
    }

    http.end();
}

// ── Setup & Loop ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);  // give USB CDC time to init on ESP32-S3
    Serial.println("\n=== Pony P1 - Phase 1: Network Test (Arduino IDE) ===");

    // Init SPI on custom pins, then init display
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);  // SCLK, MISO (unused), MOSI, SS
    tft.initR(INITR_GREENTAB);   // ST7735S 128x160 green tab
    tft.setRotation(1);          // Landscape: 160x128
    tft.fillScreen(COLOR_BLACK);
    displayStatus("Pony P1", "Booting...", COLOR_CYAN);
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
        displayStatus("WiFi lost", "Reconnecting...", COLOR_YELLOW);
        connectWiFi();
    }

    delay(50);
}
