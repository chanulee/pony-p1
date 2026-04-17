/**
 * Pony P1 - Phase 2: Push-to-talk voice -> LLM (Arduino IDE version)
 *
 * Hold the BOOT button (GPIO 0) to record audio from the SPH0645 I2S mic.
 * Release the button to POST the WAV to the Mac server, which transcribes
 * with Whisper and forwards the text to a local Ollama model.
 * The answer is shown on the ST7735S TFT.
 *
 * Required Arduino libraries:
 *   - Adafruit GFX Library
 *   - Adafruit ST7735 and ST7789 Library
 *   - ArduinoJson
 *   (ESP_I2S, WiFi, HTTPClient come with esp32 core 3.x)
 *
 * Board: ESP32S3 Dev Module, PSRAM: OPI PSRAM, USB CDC On Boot: Enabled.
 * Edit config.h with your WiFi credentials, Mac IP, and pin numbers.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <ESP_I2S.h>
#include "config.h"

// ── Display ──────────────────────────────────────────────────
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);

// ── Button ───────────────────────────────────────────────────
#define BUTTON_PIN 0  // BOOT button on ESP32-S3

// ── Color definitions (ST7735 uses BGR565) ───────────────────
#define COLOR_BLACK    ST77XX_BLACK
#define COLOR_WHITE    ST77XX_WHITE
#define COLOR_CYAN     ST77XX_CYAN
#define COLOR_YELLOW   ST77XX_YELLOW
#define COLOR_GREEN    ST77XX_GREEN
#define COLOR_RED      ST77XX_RED
#define COLOR_GREY     0x7BEF
#define COLOR_DARKGREY 0x4208

// ── Audio capture config ─────────────────────────────────────
static const uint32_t SAMPLE_RATE   = 16000;          // Whisper native rate
static const uint32_t MAX_SECONDS   = 10;             // cap recording length
static const size_t   MAX_SAMPLES   = SAMPLE_RATE * MAX_SECONDS;
static const size_t   CHUNK_SAMPLES = 512;            // ~32 ms per read @ 16 kHz

// SPH0645 produces 24-bit audio in a 32-bit I2S frame. We right-shift the
// int32 samples into int16 range. Pick by watching the Serial log for the
// "raw32 min/max" values after each recording:
//   - if |max| > 1e9  -> shift should be ~16 (huge raw values)
//   - if |max| ~ 1e6  -> shift should be ~5 (small raw values, amplify)
//   - goal: int16 peaks land in ~8000..28000 (not pinned at ±32767, not <500)
// We clip to int16 bounds, so too-small a shift just clips; too-large makes
// it quiet.
static const int      SPH0645_SHIFT = 15;

I2SClass i2s;

// PSRAM buffer for captured 16-bit mono PCM @ 16 kHz.
static int16_t* audioBuf = nullptr;
static size_t   audioSamples = 0;
static int      requestCount = 0;

// ── Display helpers ──────────────────────────────────────────
void displayStatus(const char* line1, const char* line2, uint16_t color) {
    tft.fillScreen(COLOR_BLACK);
    tft.setTextColor(color);
    tft.setTextSize(1);
    tft.setCursor(4, 10);
    tft.println(line1);

    if (line2 && strlen(line2) > 0) {
        tft.setTextColor(COLOR_GREY);
        tft.setCursor(4, 28);
        tft.setTextWrap(true);
        tft.println(line2);
    }
}

void displayResponse(const char* label, const char* body) {
    tft.fillScreen(COLOR_BLACK);
    tft.setTextColor(COLOR_CYAN);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.println(label);

    tft.drawFastHLine(0, 16, tft.width(), COLOR_DARKGREY);

    tft.setTextColor(COLOR_WHITE);
    tft.setCursor(4, 22);
    tft.setTextWrap(true);
    tft.print(body);

    tft.drawFastHLine(0, tft.height() - 12, tft.width(), COLOR_DARKGREY);
    tft.setTextColor(COLOR_DARKGREY);
    tft.setCursor(4, tft.height() - 10);
    tft.print("Hold BTN to talk");
}

void displayRecording(float seconds) {
    tft.fillScreen(COLOR_BLACK);
    tft.setTextColor(COLOR_RED);
    tft.setTextSize(2);
    tft.setCursor(4, 10);
    tft.print("REC");
    tft.fillCircle(60, 18, 5, COLOR_RED);

    tft.setTextSize(1);
    tft.setTextColor(COLOR_WHITE);
    tft.setCursor(4, 40);
    tft.printf("%.1fs", seconds);

    tft.setTextColor(COLOR_GREY);
    tft.setCursor(4, tft.height() - 10);
    tft.print("Release to send");
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
        delay(800);
    } else {
        Serial.println("\nWiFi FAILED");
        displayStatus("WiFi FAILED", "Check config.h", COLOR_RED);
        while (true) { delay(1000); }
    }
}

// ── I2S init (SPH0645, 32-bit slots, mono LEFT) ──────────────
bool initI2S() {
    // setPins(bclk, ws, dout, din, mclk)
    // For RX from SPH0645 we only use bclk + ws + din; dout/mclk are unused.
    i2s.setPins(I2S_BCLK_PIN, I2S_LRCL_PIN, -1 /*dout*/, I2S_DOUT_PIN, -1 /*mclk*/);

    // SPH0645 needs 32-bit slots. We read raw int32 and shift down to int16.
    if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE,
                   I2S_DATA_BIT_WIDTH_32BIT,
                   I2S_SLOT_MODE_MONO,
                   I2S_STD_SLOT_LEFT)) {
        Serial.println("I2S begin FAILED");
        return false;
    }
    Serial.println("I2S ready");
    return true;
}

// ── Recording ────────────────────────────────────────────────
bool allocAudioBuf() {
    if (audioBuf) return true;
    audioBuf = (int16_t*) ps_malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!audioBuf) {
        Serial.println("ps_malloc failed; falling back to heap");
        audioBuf = (int16_t*) malloc(MAX_SAMPLES * sizeof(int16_t));
    }
    return audioBuf != nullptr;
}

// Reads I2S while BOOT is held; fills audioBuf with int16 PCM @ 16 kHz.
// Returns number of samples captured.
size_t recordWhileHeld() {
    if (!allocAudioBuf()) {
        displayStatus("Record FAIL", "No PSRAM?", COLOR_RED);
        return 0;
    }

    int32_t chunk[CHUNK_SAMPLES];
    audioSamples = 0;
    uint32_t startMs = millis();
    uint32_t lastUiMs = 0;

    // Diagnostics across the whole recording.
    int32_t raw32Min = INT32_MAX;
    int32_t raw32Max = INT32_MIN;
    int16_t pcmMin   = INT16_MAX;
    int16_t pcmMax   = INT16_MIN;
    uint32_t clipCount = 0;

    while (digitalRead(BUTTON_PIN) == LOW && audioSamples < MAX_SAMPLES) {
        size_t wantBytes = CHUNK_SAMPLES * sizeof(int32_t);
        size_t gotBytes  = i2s.readBytes((char*)chunk, wantBytes);
        size_t gotSamples = gotBytes / sizeof(int32_t);

        size_t room = MAX_SAMPLES - audioSamples;
        if (gotSamples > room) gotSamples = room;

        for (size_t i = 0; i < gotSamples; i++) {
            int32_t raw = chunk[i];
            if (raw < raw32Min) raw32Min = raw;
            if (raw > raw32Max) raw32Max = raw;

            int32_t s = raw >> SPH0645_SHIFT;
            if (s >  32767) { s =  32767; clipCount++; }
            if (s < -32768) { s = -32768; clipCount++; }
            int16_t pcm = (int16_t) s;
            if (pcm < pcmMin) pcmMin = pcm;
            if (pcm > pcmMax) pcmMax = pcm;
            audioBuf[audioSamples + i] = pcm;
        }
        audioSamples += gotSamples;

        uint32_t now = millis();
        if (now - lastUiMs > 100) {
            displayRecording((now - startMs) / 1000.0f);
            lastUiMs = now;
        }
    }

    uint32_t elapsed = millis() - startMs;
    Serial.printf("Captured %u samples in %u ms\n",
                  (unsigned) audioSamples, (unsigned) elapsed);
    Serial.printf("  raw32  min=%ld max=%ld\n", (long)raw32Min, (long)raw32Max);
    Serial.printf("  int16  min=%d  max=%d  clipped=%u  (shift=%d)\n",
                  pcmMin, pcmMax, (unsigned)clipCount, SPH0645_SHIFT);

    // Suggest the smallest shift S such that (rawAbs >> S) <= TARGET, so
    // we maximize headroom without clipping. TARGET ~24000 leaves a little
    // margin under int16 max (32767). Uses |raw32Min| or raw32Max, whichever
    // is larger in magnitude (SPH0645 has a DC bias, so peaks are asymmetric).
    int32_t rawAbs = raw32Max > -raw32Min ? raw32Max : -raw32Min;
    if (rawAbs > 0) {
        const uint32_t TARGET = 24000;
        uint32_t v = (uint32_t) rawAbs;
        int suggested = 0;
        while (v > TARGET) { v >>= 1; suggested++; }
        if (suggested > 24) suggested = 24;
        Serial.printf("  -> try SPH0645_SHIFT = %d  (current %d)\n",
                      suggested, SPH0645_SHIFT);
    }
    return audioSamples;
}

// ── WAV builder + upload ─────────────────────────────────────
// Writes a 44-byte canonical PCM WAV header describing a 16-bit mono clip
// at `sampleRate` Hz with `numSamples` frames.
void buildWavHeader(uint8_t* hdr, uint32_t sampleRate, uint32_t numSamples) {
    uint32_t dataBytes = numSamples * sizeof(int16_t);
    uint32_t chunkSize = 36 + dataBytes;
    uint32_t byteRate  = sampleRate * 2;    // 1 ch * 2 bytes
    uint16_t blockAlign = 2;

    memcpy(hdr + 0,  "RIFF", 4);
    memcpy(hdr + 4,  &chunkSize, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(hdr + 16, &fmtSize, 4);
    uint16_t audioFormat = 1;   // PCM
    uint16_t numCh = 1;
    uint16_t bps = 16;
    memcpy(hdr + 20, &audioFormat, 2);
    memcpy(hdr + 22, &numCh, 2);
    memcpy(hdr + 24, &sampleRate, 4);
    memcpy(hdr + 28, &byteRate, 4);
    memcpy(hdr + 32, &blockAlign, 2);
    memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &dataBytes, 4);
}

// Show the transcript at top with a "Thinking..." footer while the LLM runs.
void displayTranscriptPending(const char* header, const char* transcript) {
    tft.fillScreen(COLOR_BLACK);

    tft.setTextColor(COLOR_CYAN);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.println(header);
    tft.drawFastHLine(0, 16, tft.width(), COLOR_DARKGREY);

    tft.setTextColor(COLOR_WHITE);
    tft.setCursor(4, 22);
    tft.setTextWrap(true);
    tft.print("You: ");
    tft.print(transcript);

    tft.drawFastHLine(0, tft.height() - 12, tft.width(), COLOR_DARKGREY);
    tft.setTextColor(COLOR_YELLOW);
    tft.setCursor(4, tft.height() - 10);
    tft.print("Thinking...");
}

// POST the prompt to /api/llm and update the display with the answer.
void callLLM(const char* transcript, const char* header) {
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/llm", SERVER_HOST, SERVER_PORT);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(5000);
    http.setTimeout(120000);

    JsonDocument req;
    req["prompt"]     = transcript;
    req["request_id"] = requestCount;
    req["uptime_ms"]  = millis();
    String body;
    serializeJson(req, body);

    Serial.printf("POST %s  prompt=%s\n", url, transcript);
    int code = http.POST(body);

    if (code == 200) {
        String response = http.getString();
        Serial.printf("LLM HTTP 200: %s\n", response.c_str());

        JsonDocument resDoc;
        if (!deserializeJson(resDoc, response) &&
            resDoc["display_text"].is<const char*>()) {
            // Build "You: <transcript>\n\n<answer>" for the full-response screen.
            const char* answer = resDoc["display_text"];
            String combined = String("You: ") + transcript + "\n\n" + answer;
            displayResponse(header, combined.c_str());
        } else {
            displayResponse(header, response.c_str());
        }
    } else if (code > 0) {
        Serial.printf("LLM HTTP %d\n", code);
        // Keep the transcript on screen and just tell the user the LLM failed.
        tft.fillScreen(COLOR_BLACK);
        tft.setTextColor(COLOR_CYAN);
        tft.setCursor(4, 4);  tft.println(header);
        tft.drawFastHLine(0, 16, tft.width(), COLOR_DARKGREY);
        tft.setTextColor(COLOR_WHITE);
        tft.setCursor(4, 22); tft.setTextWrap(true);
        tft.print("You: "); tft.println(transcript);
        tft.setTextColor(COLOR_RED);
        tft.printf("\nLLM error (HTTP %d)", code);
    } else {
        Serial.printf("LLM request failed: %s\n",
                      http.errorToString(code).c_str());
        tft.fillScreen(COLOR_BLACK);
        tft.setTextColor(COLOR_CYAN);
        tft.setCursor(4, 4);  tft.println(header);
        tft.drawFastHLine(0, 16, tft.width(), COLOR_DARKGREY);
        tft.setTextColor(COLOR_WHITE);
        tft.setCursor(4, 22); tft.setTextWrap(true);
        tft.print("You: "); tft.println(transcript);
        tft.setTextColor(COLOR_RED);
        tft.printf("\nLLM: %s", http.errorToString(code).c_str());
    }
    http.end();
}

// Two-stage pipeline: /api/stt first (fast, shows transcript ASAP),
// then /api/llm with the transcript (shows final answer).
void sendVoiceRequest() {
    requestCount++;

    if (audioSamples < SAMPLE_RATE / 4) {
        displayStatus("Too short", "Hold BOOT longer", COLOR_YELLOW);
        return;
    }

    float audioSec = (float) audioSamples / SAMPLE_RATE;
    char header[40];
    snprintf(header, sizeof(header), "#%d  %.1fs", requestCount, audioSec);

    // Build WAV buffer once; reused only for the STT POST.
    const size_t dataBytes = audioSamples * sizeof(int16_t);
    const size_t wavBytes  = 44 + dataBytes;
    uint8_t* wav = (uint8_t*) ps_malloc(wavBytes);
    if (!wav) wav = (uint8_t*) malloc(wavBytes);
    if (!wav) {
        displayStatus("OOM", "wav buffer", COLOR_RED);
        return;
    }
    buildWavHeader(wav, SAMPLE_RATE, audioSamples);
    memcpy(wav + 44, audioBuf, dataBytes);

    // ── Stage 1: STT ─────────────────────────────────────────
    displayStatus("Hearing...", "STT", COLOR_YELLOW);

    HTTPClient http;
    char url[160];
    snprintf(url, sizeof(url),
             "http://%s:%d/api/stt?request_id=%d&uptime_ms=%lu",
             SERVER_HOST, SERVER_PORT, requestCount, (unsigned long) millis());

    Serial.printf("\n[%d] POST %s (%u bytes WAV, %.2fs)\n",
                  requestCount, url, (unsigned) wavBytes, audioSec);

    http.begin(url);
    http.addHeader("Content-Type", "audio/wav");
    http.setConnectTimeout(5000);
    http.setTimeout(30000);   // STT is quick

    int code = http.POST(wav, wavBytes);
    String response = http.getString();
    http.end();
    free(wav);

    if (code != 200) {
        Serial.printf("STT HTTP %d: %s\n", code, response.c_str());
        char err[64];
        if (code > 0) snprintf(err, sizeof(err), "HTTP %d", code);
        else          snprintf(err, sizeof(err), "%s",
                               HTTPClient().errorToString(code).c_str());
        displayStatus("STT Failed", err, COLOR_RED);
        return;
    }

    Serial.printf("STT HTTP 200: %s\n", response.c_str());
    JsonDocument resDoc;
    if (deserializeJson(resDoc, response) ||
        !resDoc["transcript"].is<const char*>()) {
        displayResponse(header, response.c_str());
        return;
    }

    const char* transcript = resDoc["transcript"];

    // Empty transcript -> no LLM call. Show the hint we got from the server.
    if (!transcript || strlen(transcript) == 0) {
        const char* fallback = resDoc["display_text"].is<const char*>()
            ? (const char*) resDoc["display_text"]
            : "(no speech detected)";
        displayResponse(header, fallback);
        return;
    }

    // ── Stage 2: LLM (transcript visible while we wait) ──────
    displayTranscriptPending(header, transcript);
    callLLM(transcript, header);
}

// ── Setup & Loop ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Pony P1 - Phase 2: Voice (Arduino IDE) ===");

    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.initR(INITR_GREENTAB);
    tft.setRotation(1);
    tft.fillScreen(COLOR_BLACK);
    displayStatus("Pony P1", "Booting...", COLOR_CYAN);
    delay(500);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    connectWiFi();

    displayStatus("Init mic...", "I2S SPH0645", COLOR_YELLOW);
    if (!initI2S()) {
        displayStatus("Mic FAIL", "Check wiring", COLOR_RED);
        while (true) { delay(1000); }
    }

    if (!allocAudioBuf()) {
        displayStatus("No buffer", "PSRAM off?", COLOR_RED);
        while (true) { delay(1000); }
    }

    displayStatus("Ready", "Hold BOOT to talk", COLOR_GREEN);
    Serial.println("Ready. Hold BOOT to record.");
}

void loop() {
    if (digitalRead(BUTTON_PIN) == LOW) {
        delay(30);   // debounce
        if (digitalRead(BUTTON_PIN) == LOW) {
            size_t n = recordWhileHeld();
            if (n > 0) {
                sendVoiceRequest();
            }
            // After UI settles, return to idle screen on next press.
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, reconnecting...");
        displayStatus("WiFi lost", "Reconnecting...", COLOR_YELLOW);
        connectWiFi();
        displayStatus("Ready", "Hold BOOT to talk", COLOR_GREEN);
    }

    delay(20);
}
