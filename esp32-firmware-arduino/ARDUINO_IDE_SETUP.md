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

To find your Mac Studio's IP (the Wi-Fi interface isn't always `en0` — on Mac Studio it's often `en1`):
```bash
# Try each interface until one prints an address:
ipconfig getifaddr en0
ipconfig getifaddr en1

# Or list every interface that has an IPv4 address:
for i in $(ifconfig -l); do a=$(ipconfig getifaddr "$i" 2>/dev/null); [ -n "$a" ] && echo "$i: $a"; done
```
You can also check **System Settings > Network > (Wi‑Fi) > Details** — the "IP address" field is what you want.

## 6. Open and Upload

1. **File > Open** and navigate to `pony_p1_network_test/pony_p1_network_test.ino`
2. Click the **Upload** button (right arrow icon)
3. Wait for compilation and upload to finish
4. Open **Tools > Serial Monitor** and set baud to **115200**

## 7. Start the Mac Server

On your Mac Studio, in a separate terminal. Homebrew's Python is externally-managed, so use a virtual environment (and note that the commands are `python3` / `pip3`, not `python` / `pip`):

```bash
cd mac-server

# First time only: create and populate a venv
python3 -m venv .venv
source .venv/bin/activate
pip3 install -r requirements.txt

# Every time after that:
source .venv/bin/activate
python3 server.py
```

If you see `zsh: command not found: pip`, you either forgot to activate the venv or you're using `pip` instead of `pip3`. If you see `error: externally-managed-environment`, you're running `pip3` outside the venv — activate it first with `source .venv/bin/activate`.

## Phase 2: Push-to-Talk Voice (SPH0645 + Whisper + Ollama)

Hold the BOOT button to record, release to send. The Mac transcribes with `faster-whisper`, feeds the transcript to Ollama, and the answer appears on the TFT.

### Mic wiring (GY‑SPH0645 → T7‑S3)

| SPH0645 pin | ESP32-S3 GPIO | Notes |
|-------------|---------------|-------|
| 3V          | 3V3           | power |
| GND         | GND           | ground |
| SEL         | _unconnected_ | leaves the mic on the LEFT channel (default) |
| BCLK        | GPIO 4        | I2S bit clock |
| DOUT        | GPIO 5        | I2S data out (mic -> ESP32) |
| LRCL        | GPIO 6        | I2S word-select (WS) |

Pin numbers live in `pony_p1_network_test/config.h` — change there if you re‑wire.

### Mac-side prereqs

The first run will download the Whisper model (`base.en`, ~150 MB) into `~/.cache/huggingface`.

```bash
cd mac-server
source .venv/bin/activate
pip3 install -r requirements.txt   # installs faster-whisper + numpy
python3 server.py
```

You should see the server log:
```
Loading faster-whisper 'base.en' (cpu/int8)...
Whisper loaded in X.Xs
...
  POST /api/voice  (WAV -> Whisper -> Ollama)
```

### Using it

1. Re‑flash the ESP32 (ino has changed: I2S capture + `/api/voice`).
2. After boot, the TFT shows `Ready / Hold BOOT to talk`.
3. **Press and hold** the BOOT button while speaking (red `REC` indicator with a running timer appears).
4. **Release** to upload. TFT shows `Uploading...` → `Thinking... / STT + LLM`.
5. Answer appears on the TFT; Serial Monitor and the server terminal show the transcript + timings.

Max recording is **10 s** per hold (`MAX_SECONDS` in the `.ino`). The audio buffer lives in PSRAM, so make sure **PSRAM: OPI PSRAM** is set in the Tools menu.

### Troubleshooting voice

- **"Mic FAIL / Check wiring"** at boot: I2S didn't start. Verify BCLK/DOUT/LRCL match your wiring and that VDD is 3V3 (not 5V).
- **All silence or constant noise in transcript**: check that SEL is not tied HIGH (that would put the mic on the RIGHT slot while we read LEFT).
- **Recording is quiet**: bump `SPH0645_SHIFT` down (e.g. 10 instead of 11) in the `.ino`. Too loud / clipping: raise it (e.g. 12‑14). We clamp to int16 so clipping is hard-limited, not wrapping.
- **Whisper says nothing**: server prints `Whisper: ...s -> ''`. Try speaking louder or closer, or lengthen the recording.
- **First voice request is slow**: first `/api/voice` triggers Whisper compilation + Ollama model load; later requests are much faster.
- **"No PSRAM?" on screen**: set **Tools > PSRAM** to `OPI PSRAM` and re‑flash; without PSRAM we can't hold 10 s of 16 kHz audio.

## Phase 1.5: LLM Round-Trip (Ollama)

The firmware now POSTs a `prompt` to `/api/llm`, the Mac server forwards it to a local Ollama model, and the response is shown on the TFT.

Prereqs on the Mac Studio:

```bash
# Install Ollama from https://ollama.com if you haven't
ollama serve &              # usually already running as a background service
ollama pull gemma4:e4b      # (or edit OLLAMA_MODEL in server.py to a model you have)
```

Quick sanity check that Ollama is reachable:

```bash
curl -s http://localhost:11434/api/tags | head
```

Then start the Pony P1 server as in step 7 and press the BOOT button on the ESP32. The display should flash "Thinking..." for a few seconds, then show the model's answer. Each BOOT press cycles through a few preset prompts (see `PROMPTS[]` in the `.ino`).

If a request times out, either the model is slow to load on the first run (the first generate can take 10–30 s while the model is loaded into RAM) or the HTTP read timeout in the firmware needs to go higher — adjust `http.setTimeout(120000)` in `sendLLMRequest()`.

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
