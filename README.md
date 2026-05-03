# Pony P1

A DIY voice assistant in the spirit of the Rabbit R1, built around a LILYGO
T7-S3 (ESP32-S3) handheld and a Mac that does the heavy lifting (speech-to-text
and a local LLM with file/vision tools).

You hold the **BOOT** button on the ESP32, talk into the I2S mic, release, and
a few seconds later your transcript and the assistant's reply appear on the
160×128 TFT. The assistant is an agent: it can list, read, and create files in
a sandboxed folder on your Mac (`~/Desktop/pony_agent/` by default), read PDFs,
and look at images.

```
┌─────────────────────┐    Wi-Fi (HTTP)   ┌────────────────────────────────┐
│  ESP32-S3 (T7-S3)   │ ─────────────────▶│  Mac server (FastAPI)          │
│  • SPH0645 I2S mic  │                   │  • /api/stt   (WAV → text)     │
│  • ST7735 TFT       │ ◀─────────────────│  • /api/llm   (text → agent)   │
│  • BOOT button      │                   │  • /api/voice (WAV → agent)    │
└─────────────────────┘                   │      ↓                         │
                                          │  Ollama (qwen3.5:9b)           │
                                          │  + tools: list / read / write  │
                                          │           PDF / image (vision) │
                                          │      ↑                         │
                                          │  Sandbox: ~/Desktop/pony_agent │
                                          └────────────────────────────────┘
```

STT runs through Mistral Voxtral by default (cloud, fast) or `faster-whisper`
locally if you prefer fully offline. The LLM and vision both run through Ollama
on your Mac, so the LLM half is fully local.

## Repo layout

```
pony-p1/
├── README.md                        ← this file
├── esp32-firmware-arduino/
│   ├── ARDUINO_IDE_SETUP.md         ← detailed Arduino IDE / wiring / phase guide
│   └── pony_p1_network_test/
│       ├── pony_p1_network_test.ino ← firmware (Wi-Fi, mic, TFT, HTTP)
│       └── config.h                 ← Wi-Fi creds, server IP, pin defs
└── mac-server/
    ├── server.py                    ← FastAPI app (STT + agent endpoints)
    ├── agent.py                     ← Ollama tool-calling agent + tools
    ├── agent_cli.py                 ← Standalone CLI to test the agent
    ├── requirements.txt
    ├── .env.example                 ← Copy to .env and fill in
    └── debug_audio/                 ← Last few uploaded WAVs (gitignored)
```

## Hardware

- **LILYGO T7-S3** (ESP32-S3, 16 MB flash, 8 MB OPI PSRAM) with the bundled
  ST7735S 160×128 SPI TFT
- **Adafruit/GY-SPH0645** I2S MEMS microphone

Mic wiring:

| SPH0645 pin | ESP32-S3 GPIO | Notes |
|-------------|---------------|-------|
| 3V          | 3V3           | power |
| GND         | GND           | ground |
| SEL         | _unconnected_ | leaves the mic on the LEFT channel |
| BCLK        | GPIO 4        | I2S bit clock |
| DOUT        | GPIO 5        | I2S data out (mic → ESP32) |
| LRCL        | GPIO 6        | I2S word-select (WS) |

Pins live in `esp32-firmware-arduino/pony_p1_network_test/config.h`.

---

## Initial setup

You only do this once per machine.

### 1. Mac server: Python environment

The server needs Python 3.11+ and a virtualenv. Homebrew Python is
externally-managed (PEP 668), so a venv is mandatory.

```bash
cd mac-server

python3 -m venv .venv
source .venv/bin/activate
pip3 install -r requirements.txt
```

> If you see `zsh: command not found: pip`, you forgot `pip3` (Homebrew Python
> doesn't symlink `pip`). If you see `error: externally-managed-environment`,
> you ran `pip3` outside the venv — `source .venv/bin/activate` first.

### 2. Mac server: `.env` (secrets + config)

```bash
cd mac-server
cp .env.example .env
# Edit .env in your editor of choice
```

Minimum settings to fill in:

| Variable | What |
|----------|------|
| `STT_BACKEND` | `mistral` (cloud, default) or `whisper` (local, fully offline) |
| `MISTRAL_API_KEY` | Required if `STT_BACKEND=mistral`. Get one at <https://console.mistral.ai> |
| `OLLAMA_MODEL` | LLM the agent uses. Default `qwen3.5:9b` (multimodal + tools) |
| `OLLAMA_VISION_MODEL` | Leave blank when your main model is multimodal |
| `AGENT_ROOT` | Sandbox folder. Default `~/Desktop/pony_agent` (created on first run) |

`.env` is gitignored. **Never commit real keys.**

### 3. Mac server: Ollama + the LLM

Install Ollama from <https://ollama.com> (macOS app + CLI), then in a terminal:

```bash
# Verify Ollama is running (the macOS app starts it automatically)
curl -s http://localhost:11434/api/tags | head

# Pull the agent model (~5 GB the first time)
ollama pull qwen3.5:9b
```

`qwen3.5:9b` is multimodal (text + image) and supports tool calling, so a
single model serves the chat agent and the `describe_image` tool. If you swap
to a text-only model (e.g. `llama3.1:8b`), set `OLLAMA_VISION_MODEL` to a
vision model like `llama3.2-vision:11b`.

### 4. ESP32 firmware: Arduino IDE

Detailed steps (board manager URL, library list, board settings, port
selection) are in [`esp32-firmware-arduino/ARDUINO_IDE_SETUP.md`](esp32-firmware-arduino/ARDUINO_IDE_SETUP.md).
Quick version:

1. Install **Arduino IDE 2.x**.
2. **File → Preferences → Additional Boards Manager URLs**:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Tools → Board → Boards Manager**: install **esp32 by Espressif Systems** (3.x).
4. **Tools → Manage Libraries**: install **Adafruit GFX Library**,
   **Adafruit ST7735 and ST7789 Library**, and **ArduinoJson**.
5. **Tools** menu:
   - Board: `ESP32S3 Dev Module`
   - USB CDC On Boot: `Enabled`
   - PSRAM: `OPI PSRAM` (required — we buffer audio in PSRAM)
   - Flash Size: `16MB (128Mb)`
   - Upload Speed: `921600`

### 5. ESP32 firmware: edit `config.h`

```c
#define WIFI_SSID     "YourWiFi"
#define WIFI_PASSWORD "YourPassword"
#define SERVER_HOST   "192.168.1.42"   // your Mac's IP
#define SERVER_PORT   8080
```

Find your Mac's IP (the Wi-Fi interface isn't always `en0` on Mac Studio):

```bash
for i in $(ifconfig -l); do
  a=$(ipconfig getifaddr "$i" 2>/dev/null)
  [ -n "$a" ] && echo "$i: $a"
done
```

Or check **System Settings → Network → Wi-Fi → Details → IP address**.

Make sure the Mac and ESP32 are on the **same 2.4 GHz Wi-Fi network**
(ESP32 doesn't speak 5 GHz). On Mac, also verify **System Settings → Network →
Firewall** allows incoming connections to `python3` (port 8080).

### 6. First flash

1. Plug the T7-S3 into your Mac via USB-C.
2. **Tools → Port** → pick `/dev/cu.usbmodem*` or `/dev/cu.SLAB_USBtoUART*`.
3. Open `pony_p1_network_test/pony_p1_network_test.ino`.
4. Click **Upload**. Watch **Serial Monitor** at **115200** baud.

If upload fails, hold the **BOOT** button, click **RST**, release **BOOT** to
force download mode, then upload again.

---

## Running the system

You always need three things up at the same time: **Ollama** (background
service, just leave the macOS app running), the **Mac server**, and the
**ESP32 firmware** (already flashed).

### Every time you want to use it

In one terminal:

```bash
cd mac-server
source .venv/bin/activate
python3 server.py
```

You should see a banner like:

```
Pony P1 server on :8080
  STT backend  : mistral (model voxtral-mini-latest)
  LLM model    : qwen3.5:9b
  Vision model : qwen3.5:9b (reused)
  Agent sandbox: /Users/you/Desktop/pony_agent
  Debug audio  : /Users/you/.../mac-server/debug_audio
```

Then power on the ESP32 (USB or battery). The TFT walks through:

```
Pony P1 / Booting...
Connecting WiFi...
Ready / Hold BOOT to talk
```

### Using it

1. **Press and hold** the BOOT button while you speak (red `REC` indicator + timer).
2. **Release** to upload. The TFT first shows the transcript ("Hearing…"),
   then "Thinking…", then the agent's reply.
3. The Serial Monitor and the server terminal both log timings (`stt_s`,
   tool calls, total agent time).

Maximum recording is **10 s** per hold (`MAX_SECONDS` in the `.ino`).

### What the agent can do

The agent (`mac-server/agent.py`) exposes these tools to the LLM, all
sandboxed to `AGENT_ROOT`:

| Tool | Purpose | Example utterance |
|------|---------|-------------------|
| `list_directory` | List files in the sandbox | "What files are in my folder?" |
| `read_text_file` | Read `.txt`, `.md`, `.json`, etc. | "Read welcome.md to me." |
| `write_text_file` | Create or overwrite any text file | "Save this as todo.txt: …" |
| `create_note` | Create a NEW Markdown note (refuses overwrite) | "Research Apollo 11 and save a note." |
| `read_pdf` | Extract text from a PDF | "Summarize the PDF in my folder." |
| `describe_image` | Vision Q&A on `.png`/`.jpg` etc. | "What's in IMG_6912.png?" |

Drop files into `~/Desktop/pony_agent/` and ask about them.

### Testing the agent without the ESP32

`mac-server/agent_cli.py` runs the same agent loop from your terminal — useful
for iterating on prompts, tools, or models without flashing the device:

```bash
cd mac-server
source .venv/bin/activate

# One-shot:
python3 agent_cli.py "list the files in my folder"
python3 agent_cli.py "describe IMG_6912.png in one sentence"
python3 agent_cli.py "research the Apollo 11 mission and save a detailed note"

# Interactive REPL:
python3 agent_cli.py
```

You'll see each tool call (`tool[1]: create_note({...})` → result preview)
followed by the final spoken-style reply.

### Testing endpoints with curl

```bash
curl http://localhost:8080/health
curl -s -X POST http://localhost:8080/api/llm \
     -H 'content-type: application/json' \
     -d '{"prompt":"list my files"}' | python3 -m json.tool
```

---

## Troubleshooting (top-level)

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `Connection refused` from ESP32 | Server not running | Start `python3 server.py`, then power-cycle the ESP32 |
| `Ollama error: All connection attempts failed` | Ollama isn't running | Open the Ollama app, or `ollama serve` in a terminal |
| `(no speech detected)` despite talking | Mic gain / wiring | See "Troubleshooting voice" in [`ARDUINO_IDE_SETUP.md`](esp32-firmware-arduino/ARDUINO_IDE_SETUP.md#troubleshooting-voice); try `afplay mac-server/debug_audio/last_*.wav` |
| Agent answer is empty | LLM ran out of `num_predict` mid tool-call | Already tuned to 4000 tokens; bump higher in `agent.py` if you ask for huge files |
| Vision tool returns "(empty response)" | `qwen3.5` thinking mode swallowed the budget | Already disabled (`think: false`); make sure your Ollama is up to date |
| `MISTRAL_API_KEY` missing | `.env` not loaded or empty | Confirm `mac-server/.env` exists and the server log shows `STT backend: mistral` |
| `error: externally-managed-environment` | Ran `pip3` without venv | `source mac-server/.venv/bin/activate` first |

For ESP32-specific issues (display blank, upload fails, Wi-Fi won't connect,
mic too quiet/loud), see the bottom of
[`esp32-firmware-arduino/ARDUINO_IDE_SETUP.md`](esp32-firmware-arduino/ARDUINO_IDE_SETUP.md#troubleshooting).

---

## Security notes

- `.env`, `.venv/`, `debug_audio/`, and `__pycache__/` are gitignored. Don't
  add them back.
- The agent sandbox is a single resolved directory; symlinks pointing outside
  are rejected by `safe_path()`. Still, treat the sandbox like a place where
  the LLM might write anything — don't point `AGENT_ROOT` at sensitive data.
- Treat any API key you commit as compromised — rotate it immediately.
