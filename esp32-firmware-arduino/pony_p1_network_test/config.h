#pragma once

// ============================================================
// WiFi Configuration - UPDATE THESE FOR YOUR NETWORK
// ============================================================
#define WIFI_SSID     "NU"
#define WIFI_PASSWORD "qlqjsdla"

// ============================================================
// Mac Server Configuration
// Find your Mac's IP: System Settings > Network, or run `ipconfig getifaddr en0`
// ============================================================
#define SERVER_HOST   "192.168.86.22"  // UPDATE to your Mac Studio's IP
#define SERVER_PORT   8080

// ============================================================
// Pin Definitions - LILYGO T7-S3
// ============================================================

// I2S Microphone (GY-SPH0645)
//   VDD  -> 3V3
//   GND  -> GND
//   SEL  -> leave unconnected (defaults to LEFT channel)
//   BCLK -> GPIO 4   (Bit Clock)
//   DOUT -> GPIO 5   (Data Out from mic)
//   LRCL -> GPIO 6   (Word Select / Left-Right Clock, a.k.a. WS)
#define I2S_BCLK_PIN   4   // Bit Clock
#define I2S_DOUT_PIN   5   // Data Out from mic
#define I2S_LRCL_PIN   6   // Word Select (Left/Right Clock, a.k.a. WS)

// TFT Display (ST7735S 128x160)
#define TFT_CS    14  // Chip Select
#define TFT_RST   11  // Reset
#define TFT_DC     7  // Data/Command
#define TFT_MOSI  13  // SPI Data
#define TFT_SCLK  12  // SPI Clock
// LED / BLK -> tied to 3V3 (always on)
