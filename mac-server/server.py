"""
Phase 1: Test server for ESP32 network round-trip.

Run on your Mac Studio:
    pip install -r requirements.txt
    python server.py

The ESP32 will POST to http://<your-mac-ip>:8080/api/test
and this server responds with text to display on the TFT.
"""

import time
from fastapi import FastAPI, Request
from pydantic import BaseModel
import uvicorn

app = FastAPI(title="Pony P1 Server")


class TestRequest(BaseModel):
    message: str
    request_id: int = 0
    uptime_ms: int = 0
    free_heap: int = 0


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/api/test")
def test_endpoint(req: TestRequest):
    """Echo back a response for the ESP32 to display."""
    print(f"\n--- Request #{req.request_id} ---")
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

    return {
        "status": "ok",
        "display_text": display_text,
        "request_id": req.request_id,
    }


if __name__ == "__main__":
    print("\n=== Pony P1 Server ===")
    print("Listening on 0.0.0.0:8080")
    print("ESP32 should POST to http://<this-mac-ip>:8080/api/test\n")
    uvicorn.run(app, host="0.0.0.0", port=8080)
