"""
Pony P1 Mac server.

Endpoints:
  GET  /health      -> liveness probe
  POST /api/test    -> echo test
  POST /api/stt     -> WAV upload -> transcript only
  POST /api/llm     -> text prompt -> Ollama agent (with filesystem/vision tools)
  POST /api/voice   -> WAV upload -> STT -> Ollama agent

Agent details live in agent.py. Config (put in mac-server/.env, see .env.example):
  STT_BACKEND, MISTRAL_API_KEY, MISTRAL_STT_MODEL,
  OLLAMA_MODEL, OLLAMA_VISION_MODEL, AGENT_ROOT

Run (from mac-server/ with venv active):
    pip3 install -r requirements.txt
    python3 server.py
"""

from __future__ import annotations

import io
import os
import time
import wave
from pathlib import Path
from typing import Optional

import httpx
import numpy as np
import uvicorn
from dotenv import load_dotenv
from fastapi import FastAPI, Request
from pydantic import BaseModel

load_dotenv()  # read mac-server/.env if present BEFORE importing agent (it reads env too)

import agent  # noqa: E402


# ── Config ──────────────────────────────────────────────────
OLLAMA_URL   = os.getenv("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen3:8b")

STT_BACKEND       = os.getenv("STT_BACKEND", "mistral").lower()  # "mistral" or "whisper"
MISTRAL_API_KEY   = os.getenv("MISTRAL_API_KEY", "").strip()
MISTRAL_STT_MODEL = os.getenv("MISTRAL_STT_MODEL", "voxtral-mini-latest")
MISTRAL_STT_URL   = "https://api.mistral.ai/v1/audio/transcriptions"

# Only used when STT_BACKEND="whisper".
WHISPER_MODEL_NAME = "base.en"
WHISPER_DEVICE     = "cpu"
WHISPER_COMPUTE    = "int8"

MAX_DISPLAY_CHARS = 320
LLM_TIMEOUT_S     = 120.0
STT_TIMEOUT_S     = 60.0

DEBUG_AUDIO_DIR = Path(__file__).parent / "debug_audio"
DEBUG_AUDIO_DIR.mkdir(exist_ok=True)


app = FastAPI(title="Pony P1 Server")
whisper_model = None  # lazy-loaded if STT_BACKEND=whisper


# ── Models ──────────────────────────────────────────────────
class TestRequest(BaseModel):
    message: str
    request_id: int = 0
    uptime_ms: int = 0
    free_heap: int = 0


class LLMRequest(BaseModel):
    prompt: str = "Tell me a short funny story in 3 sentences."
    request_id: int = 0
    uptime_ms: int = 0
    free_heap: int = 0


# ── Helpers ─────────────────────────────────────────────────
def _truncate(text: str, n: int = MAX_DISPLAY_CHARS) -> str:
    text = text.strip()
    return text if len(text) <= n else text[: n - 3] + "..."


async def call_ollama(prompt: str, system: Optional[str] = None) -> tuple[str, float]:
    if system is None:
        system = (
            "You are a concise, witty assistant running on a tiny 160x128 display. "
            "Respond in plain text only. No markdown, no lists, no emoji. "
            "Keep it to 3 short sentences and under 300 characters total."
        )
    payload = {
        "model": OLLAMA_MODEL,
        "prompt": prompt,
        "system": system,
        "stream": False,
        "options": {"num_predict": 160, "temperature": 0.9},
    }
    t0 = time.time()
    async with httpx.AsyncClient(timeout=LLM_TIMEOUT_S) as client:
        r = await client.post(f"{OLLAMA_URL}/api/generate", json=payload)
        r.raise_for_status()
        data = r.json()
    return (data.get("response") or "").strip(), time.time() - t0


def _decode_wav(body: bytes) -> tuple[np.ndarray, int]:
    """Decode 16-bit PCM WAV -> (float32 mono in [-1,1], sample_rate)."""
    with wave.open(io.BytesIO(body), "rb") as w:
        sr = w.getframerate()
        n_ch = w.getnchannels()
        sw = w.getsampwidth()
        n_frames = w.getnframes()
        raw = w.readframes(n_frames)

    if sw != 2:
        raise ValueError(f"Expected 16-bit PCM, got sampwidth={sw}")
    audio = np.frombuffer(raw, dtype=np.int16)
    if n_ch > 1:
        audio = audio.reshape(-1, n_ch).mean(axis=1).astype(np.int16)
    return audio.astype(np.float32) / 32768.0, sr


def _audio_stats(audio: np.ndarray) -> tuple[float, float, float]:
    """Return (peak_abs, rms, dbfs) for a float32 signal in [-1,1]."""
    if audio.size == 0:
        return 0.0, 0.0, -120.0
    peak = float(np.max(np.abs(audio)))
    rms = float(np.sqrt(np.mean(audio.astype(np.float64) ** 2)))
    dbfs = 20.0 * np.log10(max(rms, 1e-9))
    return peak, rms, dbfs


async def mistral_transcribe(wav_bytes: bytes, language: str = "en") -> str:
    """Call Mistral Voxtral transcription endpoint. Returns plain text."""
    if not MISTRAL_API_KEY:
        raise RuntimeError("MISTRAL_API_KEY is not set (put it in mac-server/.env)")

    headers = {"Authorization": f"Bearer {MISTRAL_API_KEY}"}
    files = {
        "file": ("audio.wav", wav_bytes, "audio/wav"),
        "model": (None, MISTRAL_STT_MODEL),
        "language": (None, language),
    }
    async with httpx.AsyncClient(timeout=STT_TIMEOUT_S) as client:
        r = await client.post(MISTRAL_STT_URL, headers=headers, files=files)
        if r.status_code != 200:
            raise RuntimeError(f"Mistral STT {r.status_code}: {r.text[:300]}")
        data = r.json()
    # API returns { "text": "...", ... }; be defensive about shape.
    if isinstance(data, dict):
        text = data.get("text") or ""
        if not text and "segments" in data:
            text = " ".join(s.get("text", "") for s in data["segments"])
        return text.strip()
    return str(data).strip()


def whisper_transcribe(audio_f32: np.ndarray, sr: int) -> str:
    if whisper_model is None:
        raise RuntimeError("Whisper not loaded")
    if sr != 16000:
        raise RuntimeError(f"Whisper expects 16kHz, got {sr}")
    segments, _ = whisper_model.transcribe(
        audio_f32, language="en", beam_size=1, vad_filter=True
    )
    return " ".join(s.text for s in segments).strip()


# ── Lifecycle ───────────────────────────────────────────────
@app.on_event("startup")
def _startup():
    global whisper_model
    if STT_BACKEND == "whisper":
        from faster_whisper import WhisperModel
        print(f"Loading faster-whisper '{WHISPER_MODEL_NAME}' ({WHISPER_DEVICE}/{WHISPER_COMPUTE})...")
        t0 = time.time()
        whisper_model = WhisperModel(
            WHISPER_MODEL_NAME, device=WHISPER_DEVICE, compute_type=WHISPER_COMPUTE
        )
        print(f"Whisper loaded in {time.time() - t0:.1f}s")
    elif STT_BACKEND == "mistral":
        if not MISTRAL_API_KEY:
            print("!! MISTRAL_API_KEY is empty. /api/voice will fail until you set it in mac-server/.env")
        else:
            print(f"Using Mistral STT model: {MISTRAL_STT_MODEL}")
    else:
        print(f"!! Unknown STT_BACKEND={STT_BACKEND!r}. /api/voice will fail.")


# ── Endpoints ───────────────────────────────────────────────
@app.get("/health")
def health():
    return {
        "status": "ok",
        "ollama_model": OLLAMA_MODEL,
        "stt_backend": STT_BACKEND,
        "mistral_model": MISTRAL_STT_MODEL if STT_BACKEND == "mistral" else None,
        "whisper_model": WHISPER_MODEL_NAME if STT_BACKEND == "whisper" else None,
    }


@app.post("/api/test")
def test_endpoint(req: TestRequest):
    print(f"\n--- /api/test #{req.request_id} ---")
    print(f"  Message:   {req.message}")
    print(f"  Uptime:    {req.uptime_ms} ms")
    print(f"  Free heap: {req.free_heap} bytes")

    uptime_s = req.uptime_ms / 1000.0
    display_text = (
        f"Server received: '{req.message}'\n\n"
        f"Request #{req.request_id}\n"
        f"ESP32 uptime: {uptime_s:.1f}s\n"
        f"Free heap: {req.free_heap // 1024}KB\n"
        f"Server time: {time.strftime('%H:%M:%S')}\n\n"
        f"Round-trip OK!"
    )
    return {"status": "ok", "display_text": display_text, "request_id": req.request_id}


@app.post("/api/llm")
async def llm_endpoint(req: LLMRequest):
    print(f"\n--- /api/llm #{req.request_id} ---")
    print(f"  Prompt: {req.prompt}")
    try:
        text, trace, elapsed = await agent.run_agent(req.prompt)
    except Exception as e:
        print(f"  Agent error: {e}")
        return {
            "status": "error",
            "display_text": f"LLM error:\n{str(e)[:240]}",
            "request_id": req.request_id,
        }
    print(f"  Agent done in {elapsed:.1f}s, {len(trace)} tool calls, {len(text)} chars")
    return {
        "status": "ok",
        "display_text": _truncate(text),
        "request_id": req.request_id,
        "elapsed_s": round(elapsed, 2),
        "model": OLLAMA_MODEL,
        "tool_calls": len(trace),
        "tools_used": [t["tool"] for t in trace],
    }


async def _stt_from_body(body: bytes, request_id: int) -> dict:
    """Shared STT path for /api/stt and /api/voice.
    Decodes WAV, logs levels, saves debug copy, runs STT."""
    try:
        audio, sr = _decode_wav(body)
    except Exception as e:
        print(f"  WAV decode error: {e}")
        return {"ok": False, "error": f"bad audio: {e}", "display_text": f"Bad audio:\n{str(e)[:240]}"}

    duration = len(audio) / sr
    peak, rms, dbfs = _audio_stats(audio)
    print(f"  WAV: {sr} Hz mono, {duration:.2f}s ({len(audio)} samples)")
    print(f"  Level: peak={peak:.3f}  rms={rms:.4f}  dBFS={dbfs:.1f}")

    ts = time.strftime("%H%M%S")
    dump_path = DEBUG_AUDIO_DIR / f"voice_{ts}_{request_id:03d}.wav"
    try:
        dump_path.write_bytes(body)
        print(f"  Saved debug WAV -> {dump_path}")
        files = sorted(DEBUG_AUDIO_DIR.glob("voice_*.wav"), key=lambda p: p.stat().st_mtime)
        for old in files[:-20]:
            old.unlink(missing_ok=True)
    except Exception as e:
        print(f"  (couldn't save debug WAV: {e})")

    if peak < 0.005:
        print("  !! Audio is essentially silent (peak < 0.005). Mic wiring or gain is probably the issue.")

    t0 = time.time()
    try:
        if STT_BACKEND == "mistral":
            transcript = await mistral_transcribe(body)
        elif STT_BACKEND == "whisper":
            transcript = whisper_transcribe(audio, sr)
        else:
            raise RuntimeError(f"Unknown STT_BACKEND={STT_BACKEND!r}")
    except Exception as e:
        print(f"  STT error: {e}")
        return {"ok": False, "error": f"stt: {e}",
                "display_text": f"STT error:\n{str(e)[:240]}",
                "peak": round(peak, 4), "dbfs": round(dbfs, 1)}
    stt_elapsed = time.time() - t0
    print(f"  STT ({STT_BACKEND}) in {stt_elapsed:.2f}s -> {transcript!r}")

    return {
        "ok": True,
        "transcript": transcript,
        "audio_s": round(duration, 2),
        "stt_s": round(stt_elapsed, 2),
        "peak": round(peak, 4),
        "dbfs": round(dbfs, 1),
        "stt_backend": STT_BACKEND,
    }


@app.post("/api/stt")
async def stt_endpoint(request: Request):
    """Transcribe a WAV and return the text. No LLM call. Fast (~1s)."""
    request_id = int(request.query_params.get("request_id", 0))
    body = await request.body()
    print(f"\n--- /api/stt #{request_id} ---  ({len(body)} bytes)")

    result = await _stt_from_body(body, request_id)
    if not result["ok"]:
        return {
            "status": "error",
            "display_text": result.get("display_text", "STT error"),
            "request_id": request_id,
            "peak": result.get("peak"),
            "dbfs": result.get("dbfs"),
        }

    transcript = result["transcript"]
    if not transcript:
        hint = "\n(audio was silent - check mic)" if result["peak"] < 0.005 else ""
        display_text = f"(no speech detected){hint}"
    else:
        display_text = f"You: {transcript}"

    return {
        "status": "ok",
        "transcript": transcript,
        "display_text": _truncate(display_text),
        "request_id": request_id,
        "audio_s": result["audio_s"],
        "stt_s": result["stt_s"],
        "peak": result["peak"],
        "dbfs": result["dbfs"],
        "stt_backend": result["stt_backend"],
    }


@app.post("/api/voice")
async def voice_endpoint(request: Request):
    request_id = int(request.query_params.get("request_id", 0))
    body = await request.body()
    print(f"\n--- /api/voice #{request_id} ---  ({len(body)} bytes)")

    # 1) Decode + diagnostics
    try:
        audio, sr = _decode_wav(body)
    except Exception as e:
        print(f"  WAV decode error: {e}")
        return {
            "status": "error",
            "display_text": f"Bad audio:\n{str(e)[:240]}",
            "request_id": request_id,
        }
    duration = len(audio) / sr
    peak, rms, dbfs = _audio_stats(audio)
    print(f"  WAV: {sr} Hz mono, {duration:.2f}s ({len(audio)} samples)")
    print(f"  Level: peak={peak:.3f}  rms={rms:.4f}  dBFS={dbfs:.1f}")

    # Save the raw WAV for post-mortem listening. Keep last ~20.
    ts = time.strftime("%H%M%S")
    dump_path = DEBUG_AUDIO_DIR / f"voice_{ts}_{request_id:03d}.wav"
    try:
        dump_path.write_bytes(body)
        print(f"  Saved debug WAV -> {dump_path}")
        # Cap directory size: keep only the 20 newest files.
        files = sorted(DEBUG_AUDIO_DIR.glob("voice_*.wav"), key=lambda p: p.stat().st_mtime)
        for old in files[:-20]:
            old.unlink(missing_ok=True)
    except Exception as e:
        print(f"  (couldn't save debug WAV: {e})")

    # Advisory: if the audio is basically silent, STT will always return empty.
    if peak < 0.005:
        print("  !! Audio is essentially silent (peak < 0.005). "
              "Mic wiring or gain is probably the issue.")

    # 2) STT
    t0 = time.time()
    try:
        if STT_BACKEND == "mistral":
            transcript = await mistral_transcribe(body)
        elif STT_BACKEND == "whisper":
            transcript = whisper_transcribe(audio, sr)
        else:
            raise RuntimeError(f"Unknown STT_BACKEND={STT_BACKEND!r}")
    except Exception as e:
        print(f"  STT error: {e}")
        return {
            "status": "error",
            "display_text": f"STT error:\n{str(e)[:240]}",
            "request_id": request_id,
            "peak": round(peak, 4),
            "dbfs": round(dbfs, 1),
        }
    stt_elapsed = time.time() - t0
    print(f"  STT ({STT_BACKEND}) in {stt_elapsed:.2f}s -> {transcript!r}")

    if not transcript:
        hint = ""
        if peak < 0.005:
            hint = "\n(audio was silent - check mic)"
        return {
            "status": "ok",
            "transcript": "",
            "display_text": f"(no speech detected){hint}",
            "request_id": request_id,
            "stt_s": round(stt_elapsed, 2),
            "peak": round(peak, 4),
            "dbfs": round(dbfs, 1),
        }

    # 3) Agent (LLM + tools)
    try:
        answer, trace, llm_elapsed = await agent.run_agent(transcript)
    except Exception as e:
        print(f"  Agent error: {e}")
        return {
            "status": "error",
            "transcript": transcript,
            "display_text": f"You: {transcript}\n\nLLM error:\n{str(e)[:160]}",
            "request_id": request_id,
        }
    print(f"  Agent done in {llm_elapsed:.1f}s, {len(trace)} tool calls, {len(answer)} chars")

    return {
        "status": "ok",
        "transcript": transcript,
        "answer": answer,
        "display_text": _truncate(f"You: {transcript}\n\n{answer}"),
        "request_id": request_id,
        "audio_s": round(duration, 2),
        "stt_s": round(stt_elapsed, 2),
        "llm_s": round(llm_elapsed, 2),
        "peak": round(peak, 4),
        "dbfs": round(dbfs, 1),
        "stt_backend": STT_BACKEND,
        "model": OLLAMA_MODEL,
        "tool_calls": len(trace),
        "tools_used": [t["tool"] for t in trace],
    }


if __name__ == "__main__":
    print("\n=== Pony P1 Server ===")
    print("Listening on 0.0.0.0:8080")
    print("  POST /api/test   (echo)")
    print("  POST /api/stt    (WAV -> transcript)")
    print("  POST /api/llm    (text -> agent)")
    print("  POST /api/voice  (WAV -> STT -> agent)")
    print(f"LLM model:    {OLLAMA_MODEL} via {OLLAMA_URL}")
    print(f"Vision model: {agent.OLLAMA_VISION_MODEL}")
    print(f"STT backend:  {STT_BACKEND}")
    print(f"Agent sandbox: {agent.AGENT_ROOT}")
    print(f"Debug audio dir: {DEBUG_AUDIO_DIR}\n")
    # Make sure the sandbox exists before the first request.
    agent._ensure_sandbox()
    uvicorn.run(app, host="0.0.0.0", port=8080)
