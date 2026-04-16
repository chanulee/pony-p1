#pragma once

// ============================================================
// WiFi Configuration - UPDATE THESE FOR YOUR NETWORK
// ============================================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ============================================================
// Mac Server Configuration
// Find your Mac's IP: System Settings > Network, or run `ipconfig getifaddr en0`
// ============================================================
#define SERVER_HOST   "192.168.1.100"  // UPDATE to your Mac Studio's IP
#define SERVER_PORT   8080

// ============================================================
// Pin Definitions - LILYGO T7-S3
// ============================================================

// I2S Microphone (GY-SPH0645)
#define I2S_BCLK_PIN   4   // Bit Clock
#define I2S_LRCL_PIN   5   // Word Select (Left/Right Clock)
#define I2S_DOUT_PIN   6   // Data Out from mic

// TFT Display (ST7735S 128x160) - configured via tft_setup.py
// CS    = GPIO 14
// DC    = GPIO 7
// RST   = GPIO 11
// MOSI  = GPIO 13
// SCLK  = GPIO 12
// LED   = 3V3 (always on)
