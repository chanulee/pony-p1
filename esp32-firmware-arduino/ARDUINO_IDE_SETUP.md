# Arduino IDE Setup for Pony P1 (LILYGO T7-S3)

## 1. Install Arduino IDE

Download Arduino IDE 2.x from https://www.arduino.cc/en/software

## 2. Add ESP32-S3 Board Support

1. Open Arduino IDE
2. Go to **File > Preferences** (or **Arduino IDE > Settings** on Mac)
3. In **Additional Board Manager URLs**, add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
4. Click OK
5. Go to **Tools > Board > Boards Manager**
6. Search for **"esp32"**
7. Install **"esp32 by Espressif Systems"** (version 3.x)

## 3. Install Required Libraries

Go to **Tools > Manage Libraries** (or **Sketch > Include Library > Manage Libraries**) and install:

| Library | Author | What it does |
|---------|--------|-------------|
| **Adafruit GFX Library** | Adafruit | Graphics primitives (required by ST7735) |
| **Adafruit ST7735 and ST7789 Library** | Adafruit | TFT display driver |
| **ArduinoJson** | Benoit Blanchon | JSON parsing for server responses |

When prompted to install dependencies for Adafruit libraries, click **"Install All"**.

## 4. Board Configuration

Go to **Tools** menu and set:

| Setting | Value |
|---------|-------|
| **Board** | `ESP32S3 Dev Module` |
| **USB CDC On Boot** | `Enabled` |
| **Upload Speed** | `921600` |
| **USB Mode** | `Hardware CDC and JTAG` |
| **Flash Size** | `16MB (128Mb)` (or your board's flash) |
| **Partition Scheme** | `Default 4MB with spiffs` (or `16MB` if available) |
| **PSRAM** | `OPI PSRAM` |
| **Port** | _(select the port that appears when you plug in the board)_ |

### Finding the Port

- **Mac**: Look for `/dev/cu.usbmodem*` or `/dev/cu.SLAB_USBtoUART*`
- **Windows**: Look for `COM3`, `COM4`, etc. in Device Manager
- **Linux**: Look for `/dev/ttyACM0` or `/dev/ttyUSB0`

If no port appears when you plug in:
- Try a different USB cable (must be data, not charge-only)
- Try the other USB-C port on the T7-S3 (one is for USB, one for UART)
- On Mac/Linux you may need to hold BOOT button while plugging in

## 5. Configure Your Settings

Open `pony_p1_network_test/config.h` and update:

```c
#define WIFI_SSID     "YourWiFiName"
#define WIFI_PASSWORD "YourWiFiPassword"
#define SERVER_HOST   "192.168.1.xxx"  // Your Mac Studio's IP
```

To find your Mac Studio's IP:
```bash
ipconfig getifaddr en0
```

## 6. Open and Upload

1. **File > Open** and navigate to `pony_p1_network_test/pony_p1_network_test.ino`
2. Click the **Upload** button (right arrow icon)
3. Wait for compilation and upload to finish
4. Open **Tools > Serial Monitor** and set baud to **115200**

## 7. Start the Mac Server

On your Mac Studio, in a separate terminal:
```bash
cd mac-server
pip install -r requirements.txt
python server.py
```

## What You Should See

1. TFT shows "Pony P1 / Booting..."
2. TFT shows "Connecting WiFi..." then "WiFi Connected" with the ESP32's IP
3. ESP32 sends HTTP POST to your Mac server
4. TFT displays the server's response
5. Press the **Boot button (GPIO 0)** to send another request
6. Serial Monitor shows debug output for each step

## Troubleshooting

### Display is blank / white
- Check wiring: CS=14, DC=7, RST=11, MOSI=13, SCLK=12, LED=3V3
- Try `INITR_BLACKTAB` instead of `INITR_GREENTAB` in the .ino file (line in `setup()`)
- Some ST7735 modules need different tab colors. Options: `INITR_GREENTAB`, `INITR_BLACKTAB`, `INITR_REDTAB`

### Display colors are wrong / inverted
- Try changing `INITR_GREENTAB` to `INITR_BLACKTAB` or `INITR_18GREENTAB`
- You can also try adding `tft.invertDisplay(true);` after `tft.initR()`

### Display offset (content shifted)
- Some 128x160 green-tab modules have a pixel offset
- Add after initR: `tft.setColRowStart(2, 1);`

### WiFi won't connect
- Double-check SSID and password in config.h (case-sensitive)
- Make sure 2.4GHz WiFi is available (ESP32 doesn't support 5GHz)
- Move closer to the router for first test

### "Request Failed" on display
- Make sure `server.py` is running on your Mac
- Check the IP in config.h matches your Mac's actual IP
- Make sure Mac firewall allows port 8080 (System Settings > Network > Firewall)
- Both devices must be on the same WiFi network

### Upload fails
- Hold the **BOOT** button, click **RST**, then release BOOT to enter download mode
- Try the other USB port on the board
- Try a different USB cable
- Reduce upload speed to 460800 or 115200

### Serial Monitor shows garbage
- Set baud rate to 115200
- Make sure "USB CDC On Boot" is set to "Enabled" in board config
